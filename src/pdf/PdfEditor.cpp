#include "PdfEditor.h"
#include "Geometry.h"

#include <QByteArray>
#include <QHash>
#include <QUuid>
#include <vector>
#include <algorithm>
#include <cstring>
#include <utility>
#include <cmath>
#include <limits>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

namespace {

constexpr size_t kEditorStoreBytes = 64u * 1024u * 1024u;
constexpr size_t kSelectionTextPages = 4;

}

struct PdfEditor::SelectionTextCache
{
    fz_stext_page *page = nullptr;
    int pageIndex = -1;
};

namespace {

QRectF quadBounds(const fz_quad &quad)
{
    const float left = qMin(qMin(quad.ul.x, quad.ur.x), qMin(quad.ll.x, quad.lr.x));
    const float top = qMin(qMin(quad.ul.y, quad.ur.y), qMin(quad.ll.y, quad.lr.y));
    const float right = qMax(qMax(quad.ul.x, quad.ur.x), qMax(quad.ll.x, quad.lr.x));
    const float bottom = qMax(qMax(quad.ul.y, quad.ur.y), qMax(quad.ll.y, quad.lr.y));
    return QRectF(left, top, right - left, bottom - top);
}

QPolygonF quadPolygon(const fz_quad &quad)
{
    return { QPointF(quad.ul.x, quad.ul.y), QPointF(quad.ur.x, quad.ur.y),
             QPointF(quad.lr.x, quad.lr.y), QPointF(quad.ll.x, quad.ll.y) };
}

struct SearchQuadGroups {
    std::vector<std::vector<fz_quad>> hits;
};

int collectSearchQuads(fz_context *, void *opaque, int count, fz_quad *quads)
{
    auto *groups = static_cast<SearchQuadGroups *>(opaque);
    groups->hits.emplace_back(quads, quads + count);
    return 0;
}

using Geometry::segmentDistanceSquared;

struct LineSegment {
    QPointF start;
    QPointF end;
    QRectF bounds;
};

QRectF segmentBounds(const QPointF &start, const QPointF &end)
{
    return QRectF(start, end).normalized();
}

bool boundsIntersect(const QRectF &a, const QRectF &b)
{
    return a.left() <= b.right() && a.right() >= b.left()
           && a.top() <= b.bottom() && a.bottom() >= b.top();
}

// Annotations made by this app carry an NM name starting with the given prefix.
bool annotNameHasPrefix(fz_context *ctx, pdf_annot *annot, const char *prefix)
{
    const char *name = pdf_to_text_string(
        ctx, pdf_dict_gets(ctx, pdf_annot_obj(ctx, annot), "NM"));
    return name && std::strncmp(name, prefix, std::strlen(prefix)) == 0;
}

bool isAppTextAnnotation(fz_context *ctx, pdf_annot *annot)
{
    return annotNameHasPrefix(ctx, annot, "OfflinePdfReaderText:");
}

bool isAppImageAnnotation(fz_context *ctx, pdf_annot *annot)
{
    return annotNameHasPrefix(ctx, annot, "OfflinePdfReaderImage:");
}

bool applyEncryption(pdf_write_options &options, PdfEditor::Encryption mode,
                     const QByteArray &password, QString *error)
{
    switch (mode) {
    case PdfEditor::Encryption::Keep:
        return true;
    case PdfEditor::Encryption::Remove:
        options.do_encrypt = PDF_ENCRYPT_NONE;
        return true;
    case PdfEditor::Encryption::Aes256:
        if (password.isEmpty() || password.size() >= int(sizeof(options.upwd_utf8))) {
            *error = QStringLiteral("The password must be between 1 and 127 characters.");
            return false;
        }
        options.do_encrypt = PDF_ENCRYPT_AES_256;
        options.permissions = -1;
        std::memset(options.upwd_utf8, 0, sizeof(options.upwd_utf8));
        std::memset(options.opwd_utf8, 0, sizeof(options.opwd_utf8));
        std::memcpy(options.upwd_utf8, password.constData(), size_t(password.size()));
        std::memcpy(options.opwd_utf8, password.constData(), size_t(password.size()));
        return true;
    }
    return false;
}

bool pageHasWidgetAnnots(fz_context *ctx, pdf_document *doc, int pageIndex)
{
    pdf_obj *pageObj = pdf_lookup_page_obj(ctx, doc, pageIndex);
    pdf_obj *annots = pdf_dict_get(ctx, pageObj, PDF_NAME(Annots));
    const int count = pdf_array_len(ctx, annots);
    for (int i = 0; i < count; ++i) {
        pdf_obj *subtype = pdf_dict_get(ctx, pdf_array_get(ctx, annots, i),
                                        PDF_NAME(Subtype));
        if (pdf_name_eq(ctx, subtype, PDF_NAME(Widget)))
            return true;
    }
    return false;
}

class ErasePathIndex
{
public:
    explicit ErasePathIndex(QVector<LineSegment> segments)
        : m_segments(std::move(segments))
        , m_lastQuery(m_segments.size(), quint32(0))
    {
        if (!m_segments.isEmpty()) {
            double left = m_segments.first().bounds.left();
            double top = m_segments.first().bounds.top();
            double right = m_segments.first().bounds.right();
            double bottom = m_segments.first().bounds.bottom();
            for (const LineSegment &segment : std::as_const(m_segments)) {
                left = qMin(left, segment.bounds.left());
                top = qMin(top, segment.bounds.top());
                right = qMax(right, segment.bounds.right());
                bottom = qMax(bottom, segment.bounds.bottom());
            }
            m_bounds = QRectF(QPointF(left, top), QPointF(right, bottom));
        }

        m_cells = qBound(1, 2 * int(std::sqrt(double(m_segments.size()))), kMaxCells);
        m_cellWidth = qMax(m_bounds.width() / m_cells, 1e-6);
        m_cellHeight = qMax(m_bounds.height() / m_cells, 1e-6);
        m_grid.resize(m_cells * m_cells);

        for (int i = 0; i < m_segments.size(); ++i) {
            const QRectF &b = m_segments.at(i).bounds;
            const int x0 = cellX(b.left());
            const int x1 = cellX(b.right());
            const int y0 = cellY(b.top());
            const int y1 = cellY(b.bottom());
            for (int y = y0; y <= y1; ++y) {
                for (int x = x0; x <= x1; ++x)
                    m_grid[y * m_cells + x].append(i);
            }
        }
    }

    const QRectF &bounds() const { return m_bounds; }

    bool hits(const QPointF &a, const QPointF &b, double hitRadius,
              double hitRadiusSquared)
    {
        const QRectF query = segmentBounds(a, b).adjusted(
            -hitRadius, -hitRadius, hitRadius, hitRadius);
        ++m_queryId;
        const int x0 = cellX(query.left());
        const int x1 = cellX(query.right());
        const int y0 = cellY(query.top());
        const int y1 = cellY(query.bottom());
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                for (int index : m_grid.at(y * m_cells + x)) {
                    if (m_lastQuery[index] == m_queryId)
                        continue;
                    m_lastQuery[index] = m_queryId;

                    const LineSegment &segment = m_segments.at(index);
                    if (!boundsIntersect(query, segment.bounds))
                        continue;
                    if (segmentDistanceSquared(segment.start, segment.end, a, b)
                        <= hitRadiusSquared)
                        return true;
                }
            }
        }
        return false;
    }

private:
    static constexpr int kMaxCells = 64;

    int cellIndex(double position) const
    {
        if (!(position > 0.0))
            return 0;
        if (position >= m_cells - 1)
            return m_cells - 1;
        return int(position);
    }
    int cellX(double x) const { return cellIndex((x - m_bounds.left()) / m_cellWidth); }
    int cellY(double y) const { return cellIndex((y - m_bounds.top()) / m_cellHeight); }

    QVector<LineSegment> m_segments;
    QVector<quint32> m_lastQuery;
    QVector<QVector<int>> m_grid;
    QRectF m_bounds;
    int m_cells = 1;
    double m_cellWidth = 1.0;
    double m_cellHeight = 1.0;
    quint32 m_queryId = 0;
};

void readInkStroke(fz_context *ctx, pdf_annot *annot, int stroke,
                   std::vector<fz_point> &out)
{
    const int vertexCount = pdf_annot_ink_list_stroke_count(ctx, annot, stroke);
    out.clear();
    if (vertexCount > 0)
        out.reserve(size_t(vertexCount));
    for (int vertex = 0; vertex < vertexCount; ++vertex)
        out.push_back(pdf_annot_ink_list_stroke_vertex(ctx, annot, stroke, vertex));
}

bool strokeHitsPath(const std::vector<fz_point> &vertices, ErasePathIndex &path,
                    double hitRadius)
{
    if (vertices.empty())
        return false;

    double minX = vertices.front().x;
    double maxX = minX;
    double minY = vertices.front().y;
    double maxY = minY;
    for (size_t i = 1; i < vertices.size(); ++i) {
        minX = qMin(minX, double(vertices[i].x));
        maxX = qMax(maxX, double(vertices[i].x));
        minY = qMin(minY, double(vertices[i].y));
        maxY = qMax(maxY, double(vertices[i].y));
    }
    const QRectF expandedPath = path.bounds().adjusted(
        -hitRadius, -hitRadius, hitRadius, hitRadius);
    if (!boundsIntersect(QRectF(QPointF(minX, minY), QPointF(maxX, maxY)), expandedPath))
        return false;

    const double hitRadiusSquared = hitRadius * hitRadius;
    if (vertices.size() == 1) {
        const QPointF point(vertices.front().x, vertices.front().y);
        return path.hits(point, point, hitRadius, hitRadiusSquared);
    }
    for (size_t i = 1; i < vertices.size(); ++i) {
        const QPointF start(vertices[i - 1].x, vertices[i - 1].y);
        const QPointF end(vertices[i].x, vertices[i].y);
        if (path.hits(start, end, hitRadius, hitRadiusSquared))
            return true;
    }
    return false;
}

}

PdfEditor::PdfEditor()
{
    m_ctx = fz_new_context(nullptr, nullptr, kEditorStoreBytes);
}

PdfEditor::~PdfEditor()
{
    close();
    if (m_ctx)
        fz_drop_context(m_ctx);
}

void PdfEditor::close()
{
    clearSelectionTextCache();
    m_hasDigitalSignatures = false;
    if (m_doc) {
        pdf_drop_document(m_ctx, m_doc);
        m_doc = nullptr;
    }
    m_path.clear();
}

void PdfEditor::clearSelectionTextCache()
{
    if (m_ctx) {
        for (const std::unique_ptr<SelectionTextCache> &entry : m_selectionTextCache) {
            if (entry && entry->page)
                fz_drop_stext_page(m_ctx, entry->page);
        }
    }
    m_selectionTextCache.clear();
}

// Keeps a few recently used pages, most recent first, so a drag across several
// pages does not re-extract them on every mouse move.
bool PdfEditor::cacheSelectionTextPage(int pageIndex)
{
    for (size_t i = 0; i < m_selectionTextCache.size(); ++i) {
        if (m_selectionTextCache[i]->pageIndex == pageIndex) {
            std::rotate(m_selectionTextCache.begin(), m_selectionTextCache.begin() + i,
                        m_selectionTextCache.begin() + i + 1);
            return true;
        }
    }

    auto cache = std::make_unique<SelectionTextCache>();
    fz_page *page = nullptr;
    fz_stext_page *textPage = nullptr;
    int succeeded = 0;
    fz_var(page);
    fz_var(textPage);
    fz_var(succeeded);

    fz_try(m_ctx)
    {
        page = fz_load_page(m_ctx, &m_doc->super, pageIndex);
        textPage = fz_new_stext_page_from_page(m_ctx, page, nullptr);
        succeeded = 1;
    }
    fz_always(m_ctx)
    {
        if (page)
            fz_drop_page(m_ctx, page);
        if (!succeeded && textPage)
            fz_drop_stext_page(m_ctx, textPage);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }

    cache->page = textPage;
    cache->pageIndex = pageIndex;
    m_selectionTextCache.insert(m_selectionTextCache.begin(), std::move(cache));
    while (m_selectionTextCache.size() > kSelectionTextPages) {
        fz_drop_stext_page(m_ctx, m_selectionTextCache.back()->page);
        m_selectionTextCache.pop_back();
    }
    return true;
}

bool PdfEditor::open(const QString &path, const QString &password)
{
    close();
    m_error.clear();
    if (!m_ctx) {
        m_error = "Could not start the PDF engine.";
        return false;
    }

    const QByteArray pathUtf8 = path.toUtf8();
    const QByteArray passUtf8 = password.toUtf8();
    const char *cPath = pathUtf8.constData();
    const char *cPass = passUtf8.constData();

    pdf_document *doc = nullptr;
    int signatureCount = 0;
    fz_var(doc);
    fz_var(signatureCount);

    fz_try(m_ctx)
    {
        doc = pdf_open_document(m_ctx, cPath);
        if (fz_needs_password(m_ctx, &doc->super)
            && !fz_authenticate_password(m_ctx, &doc->super, cPass))
            fz_throw(m_ctx, FZ_ERROR_GENERIC, "Incorrect or missing password");
        signatureCount = pdf_count_signatures(m_ctx, doc);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        pdf_drop_document(m_ctx, doc);
        return false;
    }

    m_doc = doc;
    m_path = path;
    m_hasDigitalSignatures = signatureCount > 0;
    return true;
}

int PdfEditor::pageCount()
{
    if (!m_doc)
        return -1;

    int count = -1;
    fz_var(count);
    fz_try(m_ctx)
    {
        count = pdf_count_pages(m_ctx, m_doc);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return -1;
    }
    return count;
}

QVector<QSizeF> PdfEditor::pageSizes()
{
    m_error.clear();
    QVector<QSizeF> sizes;
    const int count = pageCount();
    if (count < 0)
        return sizes;
    sizes.reserve(count);
    // Read each box from the page dictionary; loading a full page would also
    // parse annotations and widgets.
    fz_try(m_ctx)
    {
        for (int page = 0; page < count; ++page) {
            pdf_obj *pageObject = pdf_lookup_page_obj(m_ctx, m_doc, page);
            fz_rect box = fz_empty_rect;
            fz_matrix ctm = fz_identity;
            pdf_page_obj_transform(m_ctx, pageObject, &box, &ctm);
            box = fz_transform_rect(box, ctm);
            const QSizeF size(box.x1 - box.x0, box.y1 - box.y0);
            if (size.isEmpty())
                fz_throw(m_ctx, FZ_ERROR_GENERIC, "Page has no size.");
            sizes.append(size);
        }
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return {};
    }
    return sizes;
}

QVector<PdfEditor::Link> PdfEditor::linksOnPage(int pageIndex)
{
    QVector<Link> links;
    m_error.clear();
    if (!m_doc) {
        m_error = QStringLiteral("No document is open.");
        return links;
    }

    fz_page *page = nullptr;
    fz_link *pageLinks = nullptr;
    fz_var(page);
    fz_var(pageLinks);
    fz_var(links);
    fz_try(m_ctx)
    {
        page = fz_load_page(m_ctx, &m_doc->super, pageIndex);
        pageLinks = fz_load_links(m_ctx, page);
        for (fz_link *item = pageLinks; item; item = item->next) {
            links.append(Link());
            Link &link = links.last();
            link.bounds = QRectF(item->rect.x0, item->rect.y0,
                                 item->rect.x1 - item->rect.x0,
                                 item->rect.y1 - item->rect.y0);
            if (item->uri) {
                if (fz_is_external_link(m_ctx, item->uri)) {
                    link.url = QUrl(QString::fromUtf8(item->uri));
                } else {
                    float x = 0.0f;
                    float y = 0.0f;
                    const fz_location destination =
                        fz_resolve_link(m_ctx, &m_doc->super, item->uri, &x, &y);
                    link.page = fz_page_number_from_location(
                        m_ctx, &m_doc->super, destination);
                    link.location = QPointF(x, y);
                }
            }
        }
    }
    fz_always(m_ctx)
    {
        fz_drop_link(m_ctx, pageLinks);
        if (page)
            fz_drop_page(m_ctx, page);
    }
    fz_catch(m_ctx)
    {
        links.clear();
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
    }
    return links;
}

QVector<PdfEditor::SearchResult> PdfEditor::search(
    const QString &text, const std::function<bool()> &isCancelled)
{
    QVector<SearchResult> results;
    m_error.clear();
    if (!m_doc) {
        m_error = QStringLiteral("No document is open.");
        return results;
    }
    const QString needleText = text.trimmed();
    if (needleText.isEmpty())
        return results;

    const QByteArray needle = needleText.toUtf8();
    // Page text is whitespace-normalised for the context snippets, so a match
    // that wraps across a line break is still found.
    const QString contextNeedle = needleText.simplified();
    const int count = pageCount();
    if (count < 0)
        return results;

    fz_var(results);
    for (int pageIndex = 0; pageIndex < count; ++pageIndex) {
        if (isCancelled && isCancelled())
            return {};

        fz_page *page = nullptr;
        fz_stext_page *textPage = nullptr;
        char *fullText = nullptr;
        QString pageText;
        SearchQuadGroups groups;
        fz_var(page);
        fz_var(textPage);
        fz_var(fullText);
        fz_var(pageText);
        fz_var(groups);
        fz_try(m_ctx)
        {
            page = fz_load_page(m_ctx, &m_doc->super, pageIndex);
            textPage = fz_new_stext_page_from_page(m_ctx, page, nullptr);
            fz_search_stext_page_cb(m_ctx, textPage, needle.constData(),
                                    collectSearchQuads, &groups);
            if (!groups.hits.empty()) {
                const fz_rect bounds = fz_bound_page(m_ctx, page);
                fullText = fz_copy_selection(
                    m_ctx, textPage, fz_make_point(bounds.x0, bounds.y0),
                    fz_make_point(bounds.x1, bounds.y1), 0);
                pageText = QString::fromUtf8(fullText ? fullText : "").simplified();
            }
        }
        fz_always(m_ctx)
        {
            fz_free(m_ctx, fullText);
            fz_drop_stext_page(m_ctx, textPage);
            fz_drop_page(m_ctx, page);
        }
        fz_catch(m_ctx)
        {
            results.clear();
            m_error = QString::fromUtf8(fz_caught_message(m_ctx));
            return results;
        }

        int searchOffset = 0;
        for (const std::vector<fz_quad> &hit : groups.hits) {
            if (hit.empty())
                continue;
            SearchResult result;
            result.page = pageIndex;
            for (const fz_quad &quad : hit) {
                const QRectF rect = quadBounds(quad);
                result.rectangles.append(rect);
                if (result.rectangles.size() == 1)
                    result.location = rect.topLeft();
            }
            const int matchIndex = pageText.indexOf(
                contextNeedle, searchOffset, Qt::CaseInsensitive);
            if (matchIndex >= 0) {
                result.contextBefore = pageText.mid(qMax(0, matchIndex - 48),
                                                    qMin(48, matchIndex));
                result.contextAfter = pageText.mid(
                    matchIndex + contextNeedle.size(), 48);
                searchOffset = matchIndex + contextNeedle.size();
            }
            results.append(std::move(result));
        }
    }
    return results;
}

QVector<PdfEditor::Bookmark> PdfEditor::bookmarks()
{
    QVector<Bookmark> result;
    m_error.clear();
    if (!m_doc) {
        m_error = QStringLiteral("No document is open.");
        return result;
    }

    fz_outline *outline = nullptr;
    fz_var(outline);
    fz_var(result);
    fz_try(m_ctx)
    {
        outline = fz_load_outline(m_ctx, &m_doc->super);
        const auto appendItems = [this](auto &&self, fz_outline *item,
                                         QVector<Bookmark> &items) -> void {
            for (; item; item = item->next) {
                items.append(Bookmark());
                Bookmark &bookmark = items.last();
                bookmark.title = QString::fromUtf8(item->title ? item->title : "");
                bookmark.page = fz_page_number_from_location(
                    m_ctx, &m_doc->super, item->page);
                bookmark.location = QPointF(item->x, item->y);
                self(self, item->down, bookmark.children);
            }
        };
        appendItems(appendItems, outline, result);
    }
    fz_always(m_ctx)
    {
        fz_drop_outline(m_ctx, outline);
    }
    fz_catch(m_ctx)
    {
        result.clear();
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
    }
    return result;
}

PdfEditor::TextSelection PdfEditor::selectText(int pageIndex, const QPointF &start,
                                               const QPointF &end, bool snapToWords)
{
    TextSelection selection;
    m_error.clear();
    if (!m_doc) {
        m_error = QStringLiteral("No document is open.");
        return selection;
    }

    if (!cacheSelectionTextPage(pageIndex))
        return selection;

    // The page just used is at the front of the cache.
    fz_stext_page *const textPage = m_selectionTextCache.front()->page;
    char *text = nullptr;
    constexpr int kMaxSelectionQuads = 65536;
    static thread_local std::vector<fz_quad> quads;
    if (quads.size() != kMaxSelectionQuads)
        quads.resize(kMaxSelectionQuads);
    fz_var(text);
    fz_try(m_ctx)
    {
        fz_point a = fz_make_point(float(start.x()), float(start.y()));
        fz_point b = fz_make_point(float(end.x()), float(end.y()));
        if (snapToWords)
            fz_snap_selection(m_ctx, textPage, &a, &b, FZ_SELECT_WORDS);
        text = fz_copy_selection(m_ctx, textPage, a, b, 0);

        const int count = fz_highlight_selection(
            m_ctx, textPage, a, b, quads.data(), int(quads.size()));
        selection.text = QString::fromUtf8(text ? text : "");
        selection.bounds.reserve(qMax(0, count));
        for (int i = 0; i < count; ++i)
            selection.bounds.append(quadPolygon(quads[size_t(i)]));
    }
    fz_always(m_ctx)
    {
        fz_free(m_ctx, text);
    }
    fz_catch(m_ctx)
    {
        selection = {};
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
    }
    return selection;
}

PdfEditor::TextSelection PdfEditor::selectWord(int pageIndex, const QPointF &point)
{
    return selectText(pageIndex, point, point, true);
}

bool PdfEditor::saveAs(const QString &path, bool removeUnused,
                       Encryption encryption, const QString &password)
{
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }

    const QByteArray pathUtf8 = path.toUtf8();
    const QByteArray passUtf8 = password.toUtf8();
    const char *cPath = pathUtf8.constData();
    pdf_write_options opts = pdf_default_write_options;

    if (removeUnused)
        opts.do_garbage = 2;

    if (!applyEncryption(opts, encryption, passUtf8, &m_error))
        return false;

    fz_try(m_ctx)
    {
        pdf_save_document(m_ctx, m_doc, cPath, &opts);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }
    return true;
}

bool PdfEditor::rotatePage(int pageIndex, int deltaDegrees)
{
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }

    clearSelectionTextCache();
    fz_try(m_ctx)
    {
        pdf_obj *pageObj = pdf_lookup_page_obj(m_ctx, m_doc, pageIndex);

        const int current = pdf_to_int(m_ctx,
            pdf_dict_get_inheritable(m_ctx, pageObj, PDF_NAME(Rotate)));

        const int updated = ((current + deltaDegrees) % 360 + 360) % 360;

        pdf_dict_put_int(m_ctx, pageObj, PDF_NAME(Rotate), updated);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }
    return true;
}

bool PdfEditor::deletePage(int pageIndex)
{
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }

    clearSelectionTextCache();
    fz_try(m_ctx)
    {
        const int count = pdf_count_pages(m_ctx, m_doc);
        if (pageIndex < 0 || pageIndex >= count)
            fz_throw(m_ctx, FZ_ERROR_GENERIC, "That page does not exist.");
        if (count <= 1)
            fz_throw(m_ctx, FZ_ERROR_GENERIC, "A PDF must keep at least one page.");

        pdf_delete_page(m_ctx, m_doc, pageIndex);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }
    return true;
}

bool PdfEditor::movePage(int fromIndex, int toIndex)
{
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }

    int count = 0;
    fz_var(count);
    fz_try(m_ctx)
    {
        count = pdf_count_pages(m_ctx, m_doc);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }

    if (fromIndex < 0 || fromIndex >= count || toIndex < 0 || toIndex >= count) {
        m_error = "That page does not exist.";
        return false;
    }
    if (fromIndex == toIndex)
        return true;

    clearSelectionTextCache();
    std::vector<int> order;
    for (int i = 0; i < count; ++i) {
        if (i != fromIndex)
            order.push_back(i);
    }
    order.insert(order.begin() + toIndex, fromIndex);
    const int *pages = order.data();

    fz_try(m_ctx)
    {
        pdf_rearrange_pages(m_ctx, m_doc, count, pages, PDF_CLEAN_STRUCTURE_DROP);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }
    return true;
}

bool PdfEditor::optimizeAs(const QString &path, Encryption encryption,
                           const QString &password)
{
    m_error.clear();
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }

    const QByteArray pathUtf8 = path.toUtf8();
    const QByteArray passUtf8 = password.toUtf8();
    pdf_write_options opts = pdf_default_write_options;
    opts.do_garbage = 3;
    opts.do_compress = 1;
    opts.do_compress_images = 1;
    opts.do_compress_fonts = 1;

    if (!applyEncryption(opts, encryption, passUtf8, &m_error))
        return false;

    fz_try(m_ctx)
    {
        pdf_save_document(m_ctx, m_doc, pathUtf8.constData(), &opts);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }
    return true;
}

bool PdfEditor::redactAs(const QString &path, const QVector<RedactionArea> &areas,
                         Encryption encryption, const QString &password)
{
    m_error.clear();
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }
    if (areas.isEmpty()) {
        m_error = "There are no redaction areas to apply.";
        return false;
    }

    int pageCount = 0;
    fz_try(m_ctx)
    {
        pageCount = pdf_count_pages(m_ctx, m_doc);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }

    QHash<int, QVector<QRectF>> areasByPage;
    for (const RedactionArea &area : areas) {
        const QRectF &bounds = area.bounds;
        if (area.page < 0 || area.page >= pageCount
            || !std::isfinite(bounds.x()) || !std::isfinite(bounds.y())
            || !std::isfinite(bounds.width()) || !std::isfinite(bounds.height())
            || qFuzzyIsNull(bounds.width()) || qFuzzyIsNull(bounds.height())) {
            m_error = "A redaction area is invalid or refers to a page that does not exist.";
            return false;
        }
        areasByPage[area.page].append(bounds.normalized());
    }

    clearSelectionTextCache();
    for (auto it = areasByPage.cbegin(); it != areasByPage.cend(); ++it) {
        pdf_page *page = nullptr;
        pdf_annot *annot = nullptr;
        fz_var(page);
        fz_var(annot);
        fz_try(m_ctx)
        {
            page = pdf_load_page(m_ctx, m_doc, it.key());
            const fz_rect pageBounds = fz_bound_page(m_ctx, &page->super);
            for (const QRectF &bounds : it.value()) {
                fz_rect redactBounds = fz_make_rect(
                    float(bounds.left()), float(bounds.top()),
                    float(bounds.right()), float(bounds.bottom()));
                redactBounds = fz_intersect_rect(redactBounds, pageBounds);
                if (fz_is_empty_rect(redactBounds))
                    fz_throw(m_ctx, FZ_ERROR_GENERIC,
                             "A redaction area is outside page %d.", it.key() + 1);

                annot = pdf_create_annot(m_ctx, page, PDF_ANNOT_REDACT);
                pdf_set_annot_rect(m_ctx, annot, redactBounds);
                pdf_update_annot(m_ctx, annot);
                pdf_drop_annot(m_ctx, annot);
                annot = nullptr;
            }

            pdf_redact_options options = {};
            options.black_boxes = 1;
            options.image_method = PDF_REDACT_IMAGE_REMOVE;
            options.line_art = PDF_REDACT_LINE_ART_REMOVE_IF_TOUCHED;
            options.text = PDF_REDACT_TEXT_REMOVE;
            pdf_redact_page(m_ctx, m_doc, page, &options);
        }
        fz_always(m_ctx)
        {
            if (annot)
                pdf_drop_annot(m_ctx, annot);
            if (page)
                fz_drop_page(m_ctx, &page->super);
        }
        fz_catch(m_ctx)
        {
            m_error = QString::fromUtf8(fz_caught_message(m_ctx));
            return false;
        }
    }

    const QByteArray pathUtf8 = path.toUtf8();
    const QByteArray passUtf8 = password.toUtf8();
    pdf_write_options options = pdf_default_write_options;
    options.do_garbage = 3;
    options.do_compress = 1;
    options.do_compress_images = 1;
    options.do_compress_fonts = 1;

    if (!applyEncryption(options, encryption, passUtf8, &m_error))
        return false;

    fz_try(m_ctx)
    {
        pdf_save_document(m_ctx, m_doc, pathUtf8.constData(), &options);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }
    return true;
}

bool PdfEditor::exportPages(const QStringList &outputPaths, const QString &format,
                            int firstPage, int resolution)
{
    m_error.clear();
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }
    if (format != QLatin1String("png") && format != QLatin1String("svg")) {
        m_error = "The requested export format is not supported.";
        return false;
    }
    const int totalPages = pageCount();
    if (totalPages < 0)
        return false;
    if (outputPaths.isEmpty() || firstPage < 0 || firstPage >= totalPages
        || outputPaths.size() > totalPages - firstPage
        || (format == QLatin1String("png") && resolution <= 0)) {
        m_error = "The requested page range or export settings are invalid.";
        return false;
    }
    for (const QString &path : outputPaths) {
        if (path.trimmed().isEmpty()) {
            m_error = "An export output path is empty.";
            return false;
        }
    }

    const QByteArray formatUtf8 = format.toUtf8();
    for (int i = 0; i < outputPaths.size(); ++i) {
        const QByteArray outputPath = outputPaths.at(i).toUtf8();
        const QByteArray options = format == QLatin1String("png")
            ? QStringLiteral("resolution=%1").arg(resolution).toUtf8()
            : QByteArray();
        fz_page *page = nullptr;
        fz_document_writer *writer = nullptr;
        fz_device *device = nullptr;
        fz_var(page);
        fz_var(writer);
        fz_var(device);

        fz_try(m_ctx)
        {
            page = fz_load_page(m_ctx, &m_doc->super, firstPage + i);
            const fz_rect bounds = fz_bound_page(m_ctx, page);
            writer = fz_new_document_writer(m_ctx, outputPath.constData(),
                                            formatUtf8.constData(),
                                            options.isEmpty() ? nullptr : options.constData());
            device = fz_begin_page(m_ctx, writer, bounds);
            fz_run_page(m_ctx, page, device, fz_identity, nullptr);
            fz_end_page(m_ctx, writer);
            device = nullptr;
            fz_close_document_writer(m_ctx, writer);
        }
        fz_always(m_ctx)
        {
            if (writer)
                fz_drop_document_writer(m_ctx, writer);
            if (page)
                fz_drop_page(m_ctx, page);
        }
        fz_catch(m_ctx)
        {
            m_error = QStringLiteral("Could not export page %1 to %2: %3")
                          .arg(firstPage + i + 1)
                          .arg(outputPaths.at(i),
                               QString::fromUtf8(fz_caught_message(m_ctx)));
            return false;
        }
    }
    return true;
}

bool PdfEditor::keepOnlyPages(int firstIndex, int lastIndex)
{
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }

    int count = 0;
    fz_var(count);
    fz_try(m_ctx)
    {
        count = pdf_count_pages(m_ctx, m_doc);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }

    if (firstIndex < 0 || lastIndex >= count || firstIndex > lastIndex) {
        m_error = "That page range is not valid.";
        return false;
    }

    clearSelectionTextCache();
    std::vector<int> keep;
    for (int i = firstIndex; i <= lastIndex; ++i)
        keep.push_back(i);
    const int *pages = keep.data();
    const int keepCount = int(keep.size());

    fz_try(m_ctx)
    {
        pdf_rearrange_pages(m_ctx, m_doc, keepCount, pages, PDF_CLEAN_STRUCTURE_DROP);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }
    return true;
}

bool PdfEditor::insertPagesFrom(const QString &sourcePath, int insertAt)
{
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }

    const QByteArray pathUtf8 = sourcePath.toUtf8();
    const char *cPath = pathUtf8.constData();

    clearSelectionTextCache();
    pdf_document *src = nullptr;
    pdf_graft_map *map = nullptr;
    fz_var(src);
    fz_var(map);

    fz_try(m_ctx)
    {
        src = pdf_open_document(m_ctx, cPath);
        if (fz_needs_password(m_ctx, &src->super))
            fz_throw(m_ctx, FZ_ERROR_GENERIC,
                     "The PDF to insert is password-protected, which is not supported yet.");

        const int destCount = pdf_count_pages(m_ctx, m_doc);
        const int srcCount = pdf_count_pages(m_ctx, src);
        if (insertAt < 0 || insertAt > destCount)
            fz_throw(m_ctx, FZ_ERROR_GENERIC, "That position is not valid.");

        map = pdf_new_graft_map(m_ctx, m_doc);
        for (int i = 0; i < srcCount; ++i)
            pdf_graft_mapped_page(m_ctx, map, insertAt + i, src, i);
    }
    fz_always(m_ctx)
    {
        if (map)
            pdf_drop_graft_map(m_ctx, map);
        if (src)
            pdf_drop_document(m_ctx, src);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }
    return true;
}

bool PdfEditor::highlightText(const QString &text, QList<int> *changedPages,
                              const std::function<bool(int, int)> &progress)
{
    if (changedPages)
        changedPages->clear();
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }
    if (text.trimmed().isEmpty()) {
        m_error = "Nothing to search for.";
        return false;
    }

    const QByteArray needleUtf8 = text.toUtf8();
    const char *needle = needleUtf8.constData();

    int count = 0;
    fz_var(count);
    fz_try(m_ctx)
    {
        count = pdf_count_pages(m_ctx, m_doc);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }

    constexpr int kMaxHitsPerPage = 65536;
    constexpr int kSearchCapacity = kMaxHitsPerPage + 1;
    std::vector<fz_quad> quads(kSearchCapacity);
    std::vector<int> marks(kSearchCapacity);
    clearSelectionTextCache();
    for (int pageIndex = 0; pageIndex < count; ++pageIndex) {
        if (progress && !progress(pageIndex, count)) {
            m_error = "Highlighting was cancelled.";
            return false;
        }

        pdf_page *page = nullptr;
        pdf_annot *annot = nullptr;
        int hits = 0;
        fz_var(page);
        fz_var(annot);
        fz_var(hits);

        fz_try(m_ctx)
        {
            page = pdf_load_page(m_ctx, m_doc, pageIndex);
            hits = fz_search_page(m_ctx, &page->super, needle,
                                  marks.data(), quads.data(), kSearchCapacity);
            if (hits > kMaxHitsPerPage) {
                fz_throw(m_ctx, FZ_ERROR_GENERIC,
                         "Page %d has more than %d matches; highlighting was stopped.",
                         pageIndex + 1, kMaxHitsPerPage);
            }
            if (hits > 0) {
                annot = pdf_create_annot(m_ctx, page, PDF_ANNOT_HIGHLIGHT);
                const float yellow[3] = { 1.0f, 0.92f, 0.0f };
                pdf_set_annot_color(m_ctx, annot, 3, yellow);
                pdf_set_annot_quad_points(m_ctx, annot, hits, quads.data());
                pdf_update_annot(m_ctx, annot);
            }
        }
        fz_always(m_ctx)
        {
            if (annot)
                pdf_drop_annot(m_ctx, annot);
            if (page)
                fz_drop_page(m_ctx, &page->super);
        }
        fz_catch(m_ctx)
        {
            m_error = QString::fromUtf8(fz_caught_message(m_ctx));
            return false;
        }

        if (hits > 0 && changedPages)
            changedPages->append(pageIndex);
    }

    return true;
}

bool PdfEditor::addTextBox(int pageIndex, const QRectF &box, const QString &text,
                           const QString &fontName, double fontSize,
                           float red, float green, float blue)
{
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }

    const QByteArray textUtf8 = text.toUtf8();
    const QByteArray fontUtf8 = fontName.toUtf8();
    const QByteArray annotationName =
        QStringLiteral("OfflinePdfReaderText:")
        .append(QUuid::createUuid().toString(QUuid::WithoutBraces)).toUtf8();
    const char *cText = textUtf8.constData();
    const char *cFont = fontUtf8.constData();
    const float color[3] = { red, green, blue };
    const fz_rect rect = fz_make_rect(float(box.left()), float(box.top()),
                                      float(box.right()), float(box.bottom()));

    clearSelectionTextCache();
    pdf_page *page = nullptr;
    pdf_annot *annot = nullptr;
    fz_var(page);
    fz_var(annot);

    fz_try(m_ctx)
    {
        page = pdf_load_page(m_ctx, m_doc, pageIndex);

        annot = pdf_create_annot(m_ctx, page, PDF_ANNOT_FREE_TEXT);
        pdf_dict_puts_drop(m_ctx, pdf_annot_obj(m_ctx, annot), "NM",
                           pdf_new_text_string(m_ctx, annotationName.constData()));
        pdf_set_annot_rect(m_ctx, annot, rect);
        pdf_set_annot_default_appearance(m_ctx, annot, cFont, float(fontSize), 3, color);
        pdf_set_annot_contents(m_ctx, annot, cText);
        pdf_update_annot(m_ctx, annot);
    }
    fz_always(m_ctx)
    {
        if (annot)
            pdf_drop_annot(m_ctx, annot);
        if (page)
            fz_drop_page(m_ctx, &page->super);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }
    return true;
}

bool PdfEditor::textAnnotationAt(int pageIndex, const QPointF &point,
                                 int *annotationId, QRectF *bounds, QString *text,
                                 QString *fontName, double *fontSize, QColor *color)
{
    m_error.clear();
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }
    if (!annotationId || !bounds || !text || !fontName || !fontSize || !color) {
        m_error = "No text annotation result was provided.";
        return false;
    }

    pdf_page *page = nullptr;
    bool found = false;
    fz_var(page);
    fz_var(found);

    fz_try(m_ctx)
    {
        page = pdf_load_page(m_ctx, m_doc, pageIndex);
        for (pdf_annot *annot = pdf_first_annot(m_ctx, page); annot;
             annot = pdf_next_annot(m_ctx, annot)) {
            if (pdf_annot_type(m_ctx, annot) != PDF_ANNOT_FREE_TEXT
                || !isAppTextAnnotation(m_ctx, annot))
                continue;

            const fz_rect rect = pdf_annot_rect(m_ctx, annot);
            if (point.x() < rect.x0 || point.x() > rect.x1
                || point.y() < rect.y0 || point.y() > rect.y1)
                continue;

            bounds->setCoords(rect.x0, rect.y0, rect.x1, rect.y1);
            *annotationId = pdf_to_num(m_ctx, pdf_annot_obj(m_ctx, annot));
            const char *contents = pdf_annot_contents(m_ctx, annot);
            *text = QString::fromUtf8(contents ? contents : "");
            const char *appearanceFont = nullptr;
            float appearanceSize = 12.0f;
            float appearanceColor[4] = { 0, 0, 0, 1 };
            int colorComponents = 0;
            pdf_annot_default_appearance(m_ctx, annot, &appearanceFont,
                                         &appearanceSize, &colorComponents,
                                         appearanceColor);
            *fontName = QString::fromLatin1(appearanceFont ? appearanceFont : "Helv");
            *fontSize = appearanceSize;
            if (colorComponents == 1) {
                const int gray = qRound(appearanceColor[0] * 255.0f);
                *color = QColor(gray, gray, gray);
            } else if (colorComponents >= 3) {
                *color = QColor(qRound(appearanceColor[0] * 255.0f),
                                qRound(appearanceColor[1] * 255.0f),
                                qRound(appearanceColor[2] * 255.0f));
            } else {
                *color = Qt::black;
            }
            found = true;
            break;
        }
    }
    fz_always(m_ctx)
    {
        if (page)
            fz_drop_page(m_ctx, &page->super);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }
    return found;
}

namespace {

bool sameTextAnnotation(fz_context *ctx, pdf_annot *annot, int annotationId,
                        const QString &text)
{
    if (pdf_annot_type(ctx, annot) != PDF_ANNOT_FREE_TEXT
        || !isAppTextAnnotation(ctx, annot)
        || pdf_to_num(ctx, pdf_annot_obj(ctx, annot)) != annotationId)
        return false;
    const char *contents = pdf_annot_contents(ctx, annot);
    return QString::fromUtf8(contents ? contents : "") == text;
}

}

bool PdfEditor::updateTextAnnotation(int pageIndex, int annotationId,
                                     const QString &oldText, const QString &newText,
                                     const QString &fontName, double fontSize,
                                     float red, float green, float blue)
{
    m_error.clear();
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }
    if (newText.trimmed().isEmpty()) {
        m_error = "Text cannot be empty.";
        return false;
    }

    const QByteArray newTextUtf8 = newText.toUtf8();
    const QByteArray fontUtf8 = fontName.toUtf8();
    const float color[3] = { red, green, blue };
    clearSelectionTextCache();
    pdf_page *page = nullptr;
    bool found = false;
    fz_var(page);
    fz_var(found);

    fz_try(m_ctx)
    {
        page = pdf_load_page(m_ctx, m_doc, pageIndex);
        for (pdf_annot *annot = pdf_first_annot(m_ctx, page); annot;
             annot = pdf_next_annot(m_ctx, annot)) {
            if (!sameTextAnnotation(m_ctx, annot, annotationId, oldText))
                continue;
            pdf_set_annot_default_appearance(m_ctx, annot, fontUtf8.constData(),
                                             float(fontSize), 3, color);
            pdf_set_annot_contents(m_ctx, annot, newTextUtf8.constData());
            pdf_update_annot(m_ctx, annot);
            found = true;
            break;
        }
    }
    fz_always(m_ctx)
    {
        if (page)
            fz_drop_page(m_ctx, &page->super);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }
    if (!found)
        m_error = "The selected text annotation could not be found.";
    return found;
}

bool PdfEditor::setTextAnnotationBounds(int pageIndex, int annotationId,
                                        const QString &text, const QRectF &bounds)
{
    m_error.clear();
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }
    if (bounds.width() <= 0 || bounds.height() <= 0) {
        m_error = "The text box dimensions must be positive.";
        return false;
    }

    const fz_rect rect = fz_make_rect(float(bounds.left()), float(bounds.top()),
                                      float(bounds.right()), float(bounds.bottom()));
    clearSelectionTextCache();
    pdf_page *page = nullptr;
    bool found = false;
    fz_var(page);
    fz_var(found);

    fz_try(m_ctx)
    {
        page = pdf_load_page(m_ctx, m_doc, pageIndex);
        for (pdf_annot *annot = pdf_first_annot(m_ctx, page); annot;
             annot = pdf_next_annot(m_ctx, annot)) {
            if (!sameTextAnnotation(m_ctx, annot, annotationId, text))
                continue;
            pdf_set_annot_rect(m_ctx, annot, rect);
            pdf_update_annot(m_ctx, annot);
            found = true;
            break;
        }
    }
    fz_always(m_ctx)
    {
        if (page)
            fz_drop_page(m_ctx, &page->super);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }
    if (!found)
        m_error = "The selected text annotation could not be found.";
    return found;
}

bool PdfEditor::deleteTextAnnotation(int pageIndex, int annotationId, const QString &text)
{
    m_error.clear();
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }

    clearSelectionTextCache();
    pdf_page *page = nullptr;
    bool found = false;
    fz_var(page);
    fz_var(found);

    fz_try(m_ctx)
    {
        page = pdf_load_page(m_ctx, m_doc, pageIndex);
        for (pdf_annot *annot = pdf_first_annot(m_ctx, page); annot;
             annot = pdf_next_annot(m_ctx, annot)) {
            if (!sameTextAnnotation(m_ctx, annot, annotationId, text))
                continue;
            pdf_delete_annot(m_ctx, page, annot);
            found = true;
            break;
        }
    }
    fz_always(m_ctx)
    {
        if (page)
            fz_drop_page(m_ctx, &page->super);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }
    if (!found)
        m_error = "The selected text annotation could not be found.";
    return found;
}

bool PdfEditor::addInkStroke(int pageIndex, const QVector<QPointF> &points,
                             float red, float green, float blue, double width, double opacity)
{
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }
    if (points.size() < 2) {
        m_error = "The stroke is too short.";
        return false;
    }

    std::vector<fz_point> stroke;
    stroke.reserve(points.size());
    for (const QPointF &p : points)
        stroke.push_back(fz_make_point(float(p.x()), float(p.y())));
    const int count = int(stroke.size());
    const float color[3] = { red, green, blue };

    clearSelectionTextCache();
    pdf_page *page = nullptr;
    pdf_annot *annot = nullptr;
    fz_var(page);
    fz_var(annot);

    fz_try(m_ctx)
    {
        page = pdf_load_page(m_ctx, m_doc, pageIndex);

        annot = pdf_create_annot(m_ctx, page, PDF_ANNOT_INK);
        pdf_set_annot_color(m_ctx, annot, 3, color);
        pdf_set_annot_border(m_ctx, annot, float(width));
        pdf_set_annot_opacity(m_ctx, annot, float(opacity));
        pdf_add_annot_ink_list(m_ctx, annot, count, stroke.data());
        pdf_update_annot(m_ctx, annot);
    }
    fz_always(m_ctx)
    {
        if (annot)
            pdf_drop_annot(m_ctx, annot);
        if (page)
            fz_drop_page(m_ctx, &page->super);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }
    return true;
}

bool PdfEditor::inkStrokesOnPage(int pageIndex, QVector<InkStroke> *strokes)
{
    m_error.clear();
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }
    if (!strokes) {
        m_error = "No ink stroke result was provided.";
        return false;
    }

    QVector<InkStroke> result;
    InkStroke stroke;
    pdf_page *page = nullptr;
    fz_var(result);
    fz_var(stroke);
    fz_var(page);
    fz_try(m_ctx)
    {
        const int count = pdf_count_pages(m_ctx, m_doc);
        if (pageIndex < 0 || pageIndex >= count)
            fz_throw(m_ctx, FZ_ERROR_GENERIC, "That page does not exist.");

        page = pdf_load_page(m_ctx, m_doc, pageIndex);
        for (pdf_annot *annot = pdf_first_annot(m_ctx, page); annot;
             annot = pdf_next_annot(m_ctx, annot)) {
            if (pdf_annot_type(m_ctx, annot) != PDF_ANNOT_INK)
                continue;

            const double width = qMax(0.0, double(pdf_annot_border_width(m_ctx, annot)));
            const int strokeCount = pdf_annot_ink_list_count(m_ctx, annot);
            for (int strokeIndex = 0; strokeIndex < strokeCount; ++strokeIndex) {
                stroke = InkStroke();
                stroke.widthPoints = width;
                const int vertexCount = pdf_annot_ink_list_stroke_count(
                    m_ctx, annot, strokeIndex);
                stroke.points.reserve(vertexCount);
                for (int vertex = 0; vertex < vertexCount; ++vertex) {
                    const fz_point point = pdf_annot_ink_list_stroke_vertex(
                        m_ctx, annot, strokeIndex, vertex);
                    stroke.points.append(QPointF(point.x, point.y));
                }
                if (!stroke.points.isEmpty())
                    result.append(std::move(stroke));
            }
        }
    }
    fz_always(m_ctx)
    {
        if (page)
            fz_drop_page(m_ctx, &page->super);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }

    *strokes = std::move(result);
    return true;
}

bool PdfEditor::eraseInk(int pageIndex, const QVector<QPointF> &path, double radius,
                         int *annotationsRemoved)
{
    if (annotationsRemoved)
        *annotationsRemoved = 0;
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }
    if (path.isEmpty() || radius <= 0.0) {
        m_error = "The eraser path or size is invalid.";
        return false;
    }

    QVector<LineSegment> pathSegments;
    if (path.size() == 1) {
        pathSegments.append({ path.first(), path.first(),
                              segmentBounds(path.first(), path.first()) });
    } else {
        pathSegments.reserve(path.size() - 1);
        for (int point = 1; point < path.size(); ++point) {
            const QPointF start = path.at(point - 1);
            const QPointF end = path.at(point);
            pathSegments.append({ start, end, segmentBounds(start, end) });
        }
    }
    ErasePathIndex erasePath(std::move(pathSegments));

    pdf_page *page = nullptr;
    pdf_annot *annot = nullptr;
    pdf_annot *next = nullptr;
    int count = 0;
    int removed = 0;
    std::vector<fz_point> vertices;
    std::vector<int> keptStrokeCounts;
    std::vector<fz_point> keptVertices;
    clearSelectionTextCache();
    fz_var(page);
    fz_var(annot);
    fz_var(next);
    fz_var(count);
    fz_var(removed);
    fz_var(vertices);
    fz_var(keptStrokeCounts);
    fz_var(keptVertices);

    fz_try(m_ctx)
    {
        count = pdf_count_pages(m_ctx, m_doc);
        if (pageIndex < 0 || pageIndex >= count)
            fz_throw(m_ctx, FZ_ERROR_GENERIC, "That page does not exist.");

        page = pdf_load_page(m_ctx, m_doc, pageIndex);
        for (annot = pdf_first_annot(m_ctx, page); annot; annot = next) {
            next = pdf_next_annot(m_ctx, annot);
            if (pdf_annot_type(m_ctx, annot) != PDF_ANNOT_INK)
                continue;

            keptStrokeCounts.clear();
            keptVertices.clear();
            int removedFromAnnotation = 0;

            const double hitRadius =
                radius + qMax(0.0, double(pdf_annot_border_width(m_ctx, annot))) / 2.0;

            const int strokeCount = pdf_annot_ink_list_count(m_ctx, annot);
            for (int stroke = 0; stroke < strokeCount; ++stroke) {
                readInkStroke(m_ctx, annot, stroke, vertices);
                if (strokeHitsPath(vertices, erasePath, hitRadius)) {
                    ++removedFromAnnotation;
                    continue;
                }
                if (vertices.empty())
                    continue;

                keptStrokeCounts.push_back(int(vertices.size()));
                keptVertices.insert(keptVertices.end(), vertices.begin(), vertices.end());
            }

            if (removedFromAnnotation == 0)
                continue;
            if (keptStrokeCounts.empty()) {
                pdf_delete_annot(m_ctx, page, annot);
            } else {
                pdf_set_annot_ink_list(m_ctx, annot, int(keptStrokeCounts.size()),
                                       keptStrokeCounts.data(), keptVertices.data());
                pdf_update_annot(m_ctx, annot);
            }
            removed += removedFromAnnotation;
        }
    }
    fz_always(m_ctx)
    {
        if (page)
            fz_drop_page(m_ctx, &page->super);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        if (annotationsRemoved)
            *annotationsRemoved = removed;
        return false;
    }

    if (annotationsRemoved)
        *annotationsRemoved = removed;
    return true;
}

bool PdfEditor::addImageStamp(int pageIndex, const QRectF &box, const QByteArray &imageData)
{
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }
    if (imageData.isEmpty()) {
        m_error = "There is no image to add.";
        return false;
    }

    const unsigned char *data = reinterpret_cast<const unsigned char *>(imageData.constData());
    const size_t size = size_t(imageData.size());
    const fz_rect rect = fz_make_rect(float(box.left()), float(box.top()),
                                      float(box.right()), float(box.bottom()));
    const QByteArray annotationName =
        QStringLiteral("OfflinePdfReaderImage:")
            .append(QUuid::createUuid().toString(QUuid::WithoutBraces)).toUtf8();

    clearSelectionTextCache();
    pdf_page *page = nullptr;
    pdf_annot *annot = nullptr;
    fz_buffer *buffer = nullptr;
    fz_image *image = nullptr;
    fz_var(page);
    fz_var(annot);
    fz_var(buffer);
    fz_var(image);

    fz_try(m_ctx)
    {
        buffer = fz_new_buffer_from_copied_data(m_ctx, data, size);
        image = fz_new_image_from_buffer(m_ctx, buffer);
        page = pdf_load_page(m_ctx, m_doc, pageIndex);

        annot = pdf_create_annot(m_ctx, page, PDF_ANNOT_STAMP);
        pdf_dict_puts_drop(m_ctx, pdf_annot_obj(m_ctx, annot), "NM",
                           pdf_new_text_string(m_ctx, annotationName.constData()));
        pdf_set_annot_rect(m_ctx, annot, rect);
        pdf_set_annot_stamp_image(m_ctx, annot, image);
        pdf_set_annot_rect(m_ctx, annot, rect);
        pdf_update_annot(m_ctx, annot);
    }
    fz_always(m_ctx)
    {
        if (annot)
            pdf_drop_annot(m_ctx, annot);
        if (image)
            fz_drop_image(m_ctx, image);
        if (buffer)
            fz_drop_buffer(m_ctx, buffer);
        if (page)
            fz_drop_page(m_ctx, &page->super);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }
    return true;
}

bool PdfEditor::imageStampAt(int pageIndex, const QPointF &point, ImageStamp *stamp)
{
    m_error.clear();
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }
    if (!stamp) {
        m_error = "No image stamp result was provided.";
        return false;
    }

    pdf_page *page = nullptr;
    pdf_annot *annot = nullptr;
    pdf_obj *imageObject = nullptr;
    fz_image *image = nullptr;
    fz_pixmap *previewPixmap = nullptr;
    fz_device *device = nullptr;
    bool found = false;
    QString id;
    QRectF bounds;
    QImage decoded;
    fz_var(page);
    fz_var(annot);
    fz_var(imageObject);
    fz_var(image);
    fz_var(previewPixmap);
    fz_var(device);
    fz_var(found);

    fz_try(m_ctx)
    {
        page = pdf_load_page(m_ctx, m_doc, pageIndex);
        for (annot = pdf_first_annot(m_ctx, page); annot;
             annot = pdf_next_annot(m_ctx, annot)) {
            if (pdf_annot_type(m_ctx, annot) != PDF_ANNOT_STAMP
                || !isAppImageAnnotation(m_ctx, annot))
                continue;
            const fz_rect rect = pdf_annot_rect(m_ctx, annot);
            if (point.x() < rect.x0 || point.x() > rect.x1
                || point.y() < rect.y0 || point.y() > rect.y1)
                continue;

            const char *name = pdf_to_text_string(
                m_ctx, pdf_dict_gets(m_ctx, pdf_annot_obj(m_ctx, annot), "NM"));
            imageObject = pdf_annot_stamp_image_obj(m_ctx, annot);
            if (!imageObject) {
                m_error = "The selected image annotation has no embedded image.";
                break;
            }
            image = pdf_load_image(m_ctx, m_doc, imageObject);
            const int width = image->w;
            const int height = image->h;
            if (width <= 0 || height <= 0 || rect.x1 <= rect.x0 || rect.y1 <= rect.y0) {
                m_error = "The selected image could not be decoded.";
                break;
            }
            previewPixmap = fz_new_pixmap(m_ctx, fz_device_rgb(m_ctx), width, height,
                                          nullptr, 1);
            fz_clear_pixmap(m_ctx, previewPixmap);
            device = fz_new_draw_device(m_ctx, fz_identity, previewPixmap);
            fz_matrix transform = fz_translate(-rect.x0, -rect.y0);
            transform = fz_post_scale(transform, width / (rect.x1 - rect.x0),
                                      height / (rect.y1 - rect.y0));
            pdf_run_annot(m_ctx, annot, device, transform, nullptr);
            fz_close_device(m_ctx, device);
            QImage wrapper(fz_pixmap_samples(m_ctx, previewPixmap), width, height,
                           int(fz_pixmap_stride(m_ctx, previewPixmap)),
                           QImage::Format_RGBA8888_Premultiplied);
            decoded = wrapper.copy();
            if (decoded.isNull()) {
                m_error = "The selected image could not be decoded.";
                break;
            }
            id = QString::fromUtf8(name);
            bounds = QRectF(rect.x0, rect.y0, rect.x1 - rect.x0, rect.y1 - rect.y0);
            found = true;
            break;
        }
    }
    fz_always(m_ctx)
    {
        if (device)
            fz_drop_device(m_ctx, device);
        if (previewPixmap)
            fz_drop_pixmap(m_ctx, previewPixmap);
        if (image)
            fz_drop_image(m_ctx, image);
        if (page)
            fz_drop_page(m_ctx, &page->super);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }

    if (!m_error.isEmpty())
        return false;
    if (!found)
        return false;
    stamp->id = id;
    stamp->bounds = bounds;
    stamp->image = decoded;
    return true;
}

bool PdfEditor::setImageStampBounds(int pageIndex, const QString &id,
                                    const QRectF &bounds)
{
    m_error.clear();
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }
    if (bounds.width() <= 0.0 || bounds.height() <= 0.0) {
        m_error = "The image bounds must have positive dimensions.";
        return false;
    }

    const QByteArray expectedId = id.toUtf8();
    const fz_rect rect = fz_make_rect(float(bounds.left()), float(bounds.top()),
                                      float(bounds.right()), float(bounds.bottom()));
    pdf_page *page = nullptr;
    pdf_annot *annot = nullptr;
    bool found = false;
    fz_var(page);
    fz_var(annot);
    fz_var(found);
    clearSelectionTextCache();
    fz_try(m_ctx)
    {
        page = pdf_load_page(m_ctx, m_doc, pageIndex);
        for (annot = pdf_first_annot(m_ctx, page); annot;
             annot = pdf_next_annot(m_ctx, annot)) {
            const char *name = pdf_to_text_string(
                m_ctx, pdf_dict_gets(m_ctx, pdf_annot_obj(m_ctx, annot), "NM"));
            if (pdf_annot_type(m_ctx, annot) == PDF_ANNOT_STAMP
                && isAppImageAnnotation(m_ctx, annot)
                && name && expectedId == name) {
                pdf_set_annot_rect(m_ctx, annot, rect);
                pdf_update_annot(m_ctx, annot);
                found = true;
                break;
            }
        }
    }
    fz_always(m_ctx)
    {
        if (page)
            fz_drop_page(m_ctx, &page->super);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }
    if (!found)
        m_error = "The selected image annotation could not be found.";
    return found;
}

bool PdfEditor::deleteImageStamp(int pageIndex, const QString &id)
{
    m_error.clear();
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }

    const QByteArray expectedId = id.toUtf8();
    pdf_page *page = nullptr;
    pdf_annot *annot = nullptr;
    bool found = false;
    fz_var(page);
    fz_var(annot);
    fz_var(found);
    clearSelectionTextCache();
    fz_try(m_ctx)
    {
        page = pdf_load_page(m_ctx, m_doc, pageIndex);
        for (annot = pdf_first_annot(m_ctx, page); annot;
             annot = pdf_next_annot(m_ctx, annot)) {
            const char *name = pdf_to_text_string(
                m_ctx, pdf_dict_gets(m_ctx, pdf_annot_obj(m_ctx, annot), "NM"));
            if (pdf_annot_type(m_ctx, annot) == PDF_ANNOT_STAMP
                && isAppImageAnnotation(m_ctx, annot)
                && name && expectedId == name) {
                pdf_delete_annot(m_ctx, page, annot);
                found = true;
                break;
            }
        }
    }
    fz_always(m_ctx)
    {
        if (page)
            fz_drop_page(m_ctx, &page->super);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }
    if (!found)
        m_error = "The selected image annotation could not be found.";
    return found;
}

bool PdfEditor::formFieldsOnPage(int pageIndex, QVector<FormField> *fields)
{
    m_error.clear();
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }
    if (!fields) {
        m_error = "No form field list was provided.";
        return false;
    }
    fields->clear();
    
    bool hasWidgets = true;
    fz_var(hasWidgets);
    fz_try(m_ctx)
    {
        hasWidgets = pageHasWidgetAnnots(m_ctx, m_doc, pageIndex);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }
    if (!hasWidgets)
        return true;

    pdf_page *page = nullptr;
    char *fieldName = nullptr;
    int succeeded = 1;
    FormField field;
    fz_var(page);
    fz_var(fieldName);
    fz_var(succeeded);
    fz_var(field);

    fz_try(m_ctx)
    {
        page = pdf_load_page(m_ctx, m_doc, pageIndex);
        int widgetIndex = 0;
        for (pdf_annot *widget = pdf_first_widget(m_ctx, page); widget;
             widget = pdf_next_widget(m_ctx, widget), ++widgetIndex) {
            field = FormField();
            const enum pdf_widget_type widgetType = pdf_widget_type(m_ctx, widget);
            switch (widgetType) {
            case PDF_WIDGET_TYPE_TEXT:        field.type = FormFieldType::Text; break;
            case PDF_WIDGET_TYPE_CHECKBOX:    field.type = FormFieldType::CheckBox; break;
            case PDF_WIDGET_TYPE_RADIOBUTTON: field.type = FormFieldType::RadioButton; break;
            case PDF_WIDGET_TYPE_COMBOBOX:    field.type = FormFieldType::ComboBox; break;
            case PDF_WIDGET_TYPE_LISTBOX:     field.type = FormFieldType::ListBox; break;
            default: continue;
            }

            const fz_rect bounds = pdf_bound_widget(m_ctx, widget);
            field.bounds = QRectF(QPointF(bounds.x0, bounds.y0),
                                  QPointF(bounds.x1, bounds.y1)).normalized();
            field.widgetIndex = widgetIndex;
            pdf_obj *fieldObject = pdf_annot_obj(m_ctx, widget);
            fieldName = pdf_load_field_name(m_ctx, fieldObject);
            field.name = fieldName ? QString::fromUtf8(fieldName) : QString();
            if (fieldName) {
                fz_free(m_ctx, fieldName);
                fieldName = nullptr;
            }
            const char *label = pdf_field_label(m_ctx, fieldObject);
            const char *value = pdf_field_value(m_ctx, fieldObject);
            field.label = label ? QString::fromUtf8(label) : QString();
            field.value = value ? QString::fromUtf8(value) : QString();
            const int flags = pdf_field_flags(m_ctx, fieldObject);
            field.readOnly = pdf_widget_is_readonly(m_ctx, widget) != 0;
            field.multiline = (flags & PDF_TX_FIELD_IS_MULTILINE) != 0;
            field.password = (flags & PDF_TX_FIELD_IS_PASSWORD) != 0;
            field.editableChoice = field.type == FormFieldType::ComboBox
                                   && (flags & PDF_CH_FIELD_IS_EDIT) != 0;

            if (field.type == FormFieldType::CheckBox
                || field.type == FormFieldType::RadioButton) {
                pdf_obj *onState = pdf_button_field_on_state(m_ctx, fieldObject);
                const char *onValue = pdf_to_name(m_ctx, onState);
                field.checked = value && onValue && std::strcmp(value, onValue) == 0;
            } else if (field.type == FormFieldType::ComboBox
                       || field.type == FormFieldType::ListBox) {
                const int optionCount = pdf_choice_field_option_count(m_ctx, fieldObject);
                for (int i = 0; i < optionCount; ++i) {
                    const char *labelValue = pdf_choice_field_option(m_ctx, fieldObject, 0, i);
                    const char *exportValue = pdf_choice_field_option(m_ctx, fieldObject, 1, i);
                    field.options.append(labelValue ? QString::fromUtf8(labelValue) : QString());
                    field.optionValues.append(
                        exportValue ? QString::fromUtf8(exportValue) : QString());
                }
            }
            fields->append(field);
        }
        pdf_drop_page(m_ctx, page);
        page = nullptr;
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        succeeded = 0;
    }

    if (fieldName)
        fz_free(m_ctx, fieldName);
    if (page)
        pdf_drop_page(m_ctx, page);
    return succeeded != 0;
}

bool PdfEditor::formFieldAt(int pageIndex, const QPointF &point, FormField *field)
{
    m_error.clear();
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }
    if (!field) {
        m_error = "No form field result was provided.";
        return false;
    }

    *field = FormField();

    bool hasWidgets = true;
    fz_var(hasWidgets);
    fz_try(m_ctx)
    {
        hasWidgets = pageHasWidgetAnnots(m_ctx, m_doc, pageIndex);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }
    if (!hasWidgets)
        return true;

    pdf_page *page = nullptr;
    pdf_annot *matchedWidget = nullptr;
    pdf_obj *fieldObject = nullptr;
    char *fieldName = nullptr;
    const char *fieldLabel = nullptr;
    const char *fieldValue = nullptr;
    const char **labels = nullptr;
    const char **values = nullptr;
    fz_rect bounds = { 0, 0, 0, 0 };
    FormFieldType fieldType = FormFieldType::None;
    int widgetFlags = 0;
    int readOnly = 0;
    int optionCount = 0;
    int succeeded = 1;
    fz_var(page);
    fz_var(matchedWidget);
    fz_var(fieldObject);
    fz_var(fieldName);
    fz_var(fieldLabel);
    fz_var(fieldValue);
    fz_var(labels);
    fz_var(values);
    fz_var(bounds);
    fz_var(fieldType);
    fz_var(widgetFlags);
    fz_var(readOnly);
    fz_var(optionCount);
    fz_var(succeeded);

    fz_try(m_ctx)
    {
        page = pdf_load_page(m_ctx, m_doc, pageIndex);
        int widgetIndex = 0;
        for (pdf_annot *widget = pdf_first_widget(m_ctx, page); widget;
             widget = pdf_next_widget(m_ctx, widget), ++widgetIndex) {
            const enum pdf_widget_type type = pdf_widget_type(m_ctx, widget);
            fieldType = FormFieldType::None;
            switch (type) {
            case PDF_WIDGET_TYPE_TEXT:       fieldType = FormFieldType::Text; break;
            case PDF_WIDGET_TYPE_CHECKBOX:   fieldType = FormFieldType::CheckBox; break;
            case PDF_WIDGET_TYPE_RADIOBUTTON: fieldType = FormFieldType::RadioButton; break;
            case PDF_WIDGET_TYPE_COMBOBOX:   fieldType = FormFieldType::ComboBox; break;
            case PDF_WIDGET_TYPE_LISTBOX:    fieldType = FormFieldType::ListBox; break;
            default: break;
            }
            if (fieldType == FormFieldType::None)
                continue;

            bounds = pdf_bound_widget(m_ctx, widget);
            if (point.x() < qMin(bounds.x0, bounds.x1)
                || point.x() > qMax(bounds.x0, bounds.x1)
                || point.y() < qMin(bounds.y0, bounds.y1)
                || point.y() > qMax(bounds.y0, bounds.y1))
                continue;

            matchedWidget = widget;
            fieldObject = pdf_annot_obj(m_ctx, widget);
            fieldName = pdf_load_field_name(m_ctx, fieldObject);
            fieldLabel = pdf_field_label(m_ctx, fieldObject);
            fieldValue = pdf_field_value(m_ctx, fieldObject);
            widgetFlags = pdf_field_flags(m_ctx, fieldObject);
            readOnly = pdf_widget_is_readonly(m_ctx, widget);

            if (fieldType == FormFieldType::ComboBox
                || fieldType == FormFieldType::ListBox) {
                optionCount = pdf_choice_widget_options(m_ctx, widget, 0, nullptr);
                if (optionCount > 0) {
                    labels = fz_malloc_array(m_ctx, optionCount, const char *);
                    values = fz_malloc_array(m_ctx, optionCount, const char *);
                    pdf_choice_widget_options(m_ctx, widget, 0, labels);
                    pdf_choice_widget_options(m_ctx, widget, 1, values);
                }
            }

            field->widgetIndex = widgetIndex;
            break;
        }
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        succeeded = 0;
    }

    if (succeeded && matchedWidget) {
        field->type = fieldType;
        field->bounds = QRectF(QPointF(bounds.x0, bounds.y0),
                               QPointF(bounds.x1, bounds.y1)).normalized();
        field->name = fieldName ? QString::fromUtf8(fieldName) : QString();
        field->label = fieldLabel ? QString::fromUtf8(fieldLabel) : QString();
        field->value = fieldValue ? QString::fromUtf8(fieldValue) : QString();
        if (fieldType == FormFieldType::CheckBox
            || fieldType == FormFieldType::RadioButton) {
            pdf_obj *onState = pdf_button_field_on_state(m_ctx, fieldObject);
            const char *onValue = pdf_to_name(m_ctx, onState);
            field->checked = fieldValue && onValue
                             && std::strcmp(fieldValue, onValue) == 0;
        }
        field->readOnly = readOnly != 0;
        field->multiline = (widgetFlags & PDF_TX_FIELD_IS_MULTILINE) != 0;
        field->password = (widgetFlags & PDF_TX_FIELD_IS_PASSWORD) != 0;
        field->editableChoice = fieldType == FormFieldType::ComboBox
                                && (widgetFlags & PDF_CH_FIELD_IS_EDIT) != 0;
        for (int i = 0; i < optionCount; ++i) {
            field->options.append(labels[i] ? QString::fromUtf8(labels[i]) : QString());
            field->optionValues.append(values[i] ? QString::fromUtf8(values[i]) : QString());
        }
    }

    if (values)
        fz_free(m_ctx, values);
    if (labels)
        fz_free(m_ctx, labels);
    if (fieldName)
        fz_free(m_ctx, fieldName);
    if (page)
        pdf_drop_page(m_ctx, page);

    return succeeded != 0;
}

bool PdfEditor::setFormFieldValue(int pageIndex, int widgetIndex, const QString &value,
                                  bool *pageLocal)
{
    m_error.clear();
    if (pageLocal)
        *pageLocal = false;
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }
    if (widgetIndex < 0) {
        m_error = "That form field could not be found.";
        return false;
    }

    const QByteArray valueUtf8 = value.toUtf8();
    clearSelectionTextCache();
    pdf_page *page = nullptr;
    int found = 0;
    int accepted = 0;
    int localToPage = 0;
    fz_var(page);
    fz_var(found);
    fz_var(accepted);
    fz_var(localToPage);

    fz_try(m_ctx)
    {
        page = pdf_load_page(m_ctx, m_doc, pageIndex);
        int index = 0;
        for (pdf_annot *widget = pdf_first_widget(m_ctx, page); widget;
             widget = pdf_next_widget(m_ctx, widget), ++index) {
            if (index != widgetIndex)
                continue;

            found = 1;
            if (pdf_widget_is_readonly(m_ctx, widget))
                break;

            // A widget with its own /T is a complete field. Without one it is a
            // kid of a shared field (radio groups, repeated widgets), whose
            // value can change on other pages too.
            localToPage = pdf_widget_type(m_ctx, widget) != PDF_WIDGET_TYPE_RADIOBUTTON
                          && pdf_dict_get(m_ctx, pdf_annot_obj(m_ctx, widget), PDF_NAME(T));
            switch (pdf_widget_type(m_ctx, widget)) {
            case PDF_WIDGET_TYPE_TEXT:
                accepted = pdf_set_text_field_value(
                    m_ctx, widget, valueUtf8.constData());
                break;
            case PDF_WIDGET_TYPE_COMBOBOX:
            case PDF_WIDGET_TYPE_LISTBOX:
                accepted = pdf_set_choice_field_value(
                    m_ctx, widget, valueUtf8.constData());
                break;
            case PDF_WIDGET_TYPE_CHECKBOX:
            case PDF_WIDGET_TYPE_RADIOBUTTON:
                accepted = pdf_toggle_widget(m_ctx, widget);
                break;
            default:
                break;
            }
            break;
        }

        if (accepted)
            pdf_update_page(m_ctx, page);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
    }

    if (page)
        pdf_drop_page(m_ctx, page);
    if (!m_error.isEmpty())
        return false;
    if (!found) {
        m_error = "That form field could not be found.";
        return false;
    }
    if (!accepted) {
        m_error = "The PDF rejected that value or the field is read-only.";
        return false;
    }
    if (pageLocal)
        *pageLocal = localToPage != 0;
    return true;
}


