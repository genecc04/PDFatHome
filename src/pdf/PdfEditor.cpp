#include "PdfEditor.h"
#include "MuPdfRenderCache.h"
#include "RenderDocumentBytesCache.h"

#include <QByteArray>
#include <QFileInfo>
#include <QUuid>
#include <vector>
#include <cstring>
#include <utility>
#include <cmath>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

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

double pointSegmentDistanceSquared(const QPointF &point, const QPointF &start,
                                   const QPointF &end)
{
    const double dx = end.x() - start.x();
    const double dy = end.y() - start.y();
    const double lengthSquared = dx * dx + dy * dy;
    if (lengthSquared == 0.0) {
        const double px = point.x() - start.x();
        const double py = point.y() - start.y();
        return px * px + py * py;
    }

    const double projection = qBound(
        0.0, ((point.x() - start.x()) * dx + (point.y() - start.y()) * dy)
                 / lengthSquared,
        1.0);
    const double px = point.x() - (start.x() + projection * dx);
    const double py = point.y() - (start.y() + projection * dy);
    return px * px + py * py;
}

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

double crossProduct(const QPointF &a, const QPointF &b, const QPointF &c)
{
    return (b.x() - a.x()) * (c.y() - a.y())
           - (b.y() - a.y()) * (c.x() - a.x());
}

bool pointOnSegment(const QPointF &point, const QPointF &start, const QPointF &end)
{
    constexpr double epsilon = 1e-9;
    return qAbs(crossProduct(start, end, point)) <= epsilon
           && point.x() >= qMin(start.x(), end.x()) - epsilon
           && point.x() <= qMax(start.x(), end.x()) + epsilon
           && point.y() >= qMin(start.y(), end.y()) - epsilon
           && point.y() <= qMax(start.y(), end.y()) + epsilon;
}

bool segmentsIntersect(const QPointF &a, const QPointF &b,
                      const QPointF &c, const QPointF &d)
{
    const double abC = crossProduct(a, b, c);
    const double abD = crossProduct(a, b, d);
    const double cdA = crossProduct(c, d, a);
    const double cdB = crossProduct(c, d, b);
    if (((abC > 0.0 && abD < 0.0) || (abC < 0.0 && abD > 0.0))
        && ((cdA > 0.0 && cdB < 0.0) || (cdA < 0.0 && cdB > 0.0)))
        return true;
    return (abC == 0.0 && pointOnSegment(c, a, b))
           || (abD == 0.0 && pointOnSegment(d, a, b))
           || (cdA == 0.0 && pointOnSegment(a, c, d))
           || (cdB == 0.0 && pointOnSegment(b, c, d));
}

double segmentDistanceSquared(const QPointF &a, const QPointF &b,
                              const QPointF &c, const QPointF &d)
{
    if (segmentsIntersect(a, b, c, d))
        return 0.0;
    return qMin(qMin(pointSegmentDistanceSquared(a, c, d),
                     pointSegmentDistanceSquared(b, c, d)),
                qMin(pointSegmentDistanceSquared(c, a, b),
                     pointSegmentDistanceSquared(d, a, b)));
}

bool isAppTextAnnotation(fz_context *ctx, pdf_annot *annot)
{
    const char *name = pdf_to_text_string(
        ctx, pdf_dict_gets(ctx, pdf_annot_obj(ctx, annot), "NM"));
    return name && QByteArray(name).startsWith("OfflinePdfReaderText:");
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
    m_ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
}

PdfEditor::~PdfEditor()
{
    close();
    if (m_ctx)
        fz_drop_context(m_ctx);
}

void PdfEditor::close()
{
    if (m_doc) {
        pdf_drop_document(m_ctx, m_doc);
        m_doc = nullptr;
    }
    m_path.clear();
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
    fz_var(doc);

    fz_try(m_ctx)
    {
        doc = pdf_open_document(m_ctx, cPath);
        if (fz_needs_password(m_ctx, &doc->super)
            && !fz_authenticate_password(m_ctx, &doc->super, cPass))
            fz_throw(m_ctx, FZ_ERROR_GENERIC, "Incorrect or missing password");
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        pdf_drop_document(m_ctx, doc);
        return false;
    }

    m_doc = doc;
    m_path = path;
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

QSizeF PdfEditor::pagePointSize(int pageIndex)
{
    m_error.clear();
    if (!m_doc) {
        m_error = QStringLiteral("No document is open.");
        return {};
    }

    pdf_page *page = nullptr;
    fz_var(page);
    float width = 0.0f;
    float height = 0.0f;
    fz_var(width);
    fz_var(height);
    fz_try(m_ctx)
    {
        page = pdf_load_page(m_ctx, m_doc, pageIndex);
        const fz_rect bounds = fz_bound_page(m_ctx, &page->super);
        width = bounds.x1 - bounds.x0;
        height = bounds.y1 - bounds.y0;
    }
    fz_always(m_ctx)
    {
        if (page)
            fz_drop_page(m_ctx, &page->super);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return {};
    }
    return QSizeF(width, height);
}

QVector<QSizeF> PdfEditor::pageSizes()
{
    m_error.clear();
    QVector<QSizeF> sizes;
    const int count = pageCount();
    if (count < 0)
        return sizes;
    sizes.reserve(count);
    for (int page = 0; page < count; ++page) {
        const QSizeF size = pagePointSize(page);
        if (size.isEmpty())
            return {};
        sizes.append(size);
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
    fz_try(m_ctx)
    {
        page = fz_load_page(m_ctx, &m_doc->super, pageIndex);
        pageLinks = fz_load_links(m_ctx, page);
        for (fz_link *item = pageLinks; item; item = item->next) {
            Link link;
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
            links.append(link);
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

QVector<PdfEditor::SearchResult> PdfEditor::search(const QString &text)
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
    const int count = pageCount();
    if (count < 0)
        return results;

    for (int pageIndex = 0; pageIndex < count; ++pageIndex) {
        fz_page *page = nullptr;
        fz_stext_page *textPage = nullptr;
        char *fullText = nullptr;
        SearchQuadGroups groups;
        fz_var(page);
        fz_var(textPage);
        fz_var(fullText);
        fz_try(m_ctx)
        {
            page = fz_load_page(m_ctx, &m_doc->super, pageIndex);
            textPage = fz_new_stext_page_from_page(m_ctx, page, nullptr);
            fz_search_stext_page_cb(m_ctx, textPage, needle.constData(),
                                    collectSearchQuads, &groups);
            const fz_rect bounds = fz_bound_page(m_ctx, page);
            fullText = fz_copy_selection(
                m_ctx, textPage, fz_make_point(bounds.x0, bounds.y0),
                fz_make_point(bounds.x1, bounds.y1), 0);

            const QString pageText = QString::fromUtf8(fullText ? fullText : "");
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
                    needleText, searchOffset, Qt::CaseInsensitive);
                if (matchIndex >= 0) {
                    result.contextBefore = pageText.mid(qMax(0, matchIndex - 48),
                                                        qMin(48, matchIndex));
                    result.contextAfter = pageText.mid(
                        matchIndex + needleText.size(), 48);
                    searchOffset = matchIndex + needleText.size();
                }
                results.append(std::move(result));
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
    fz_try(m_ctx)
    {
        outline = fz_load_outline(m_ctx, &m_doc->super);
        const auto appendItems = [this](auto &&self, fz_outline *item,
                                         QVector<Bookmark> &items) -> void {
            for (; item; item = item->next) {
                Bookmark bookmark;
                bookmark.title = QString::fromUtf8(item->title ? item->title : "");
                bookmark.page = fz_page_number_from_location(
                    m_ctx, &m_doc->super, item->page);
                bookmark.location = QPointF(item->x, item->y);
                self(self, item->down, bookmark.children);
                items.append(std::move(bookmark));
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

    fz_page *page = nullptr;
    fz_stext_page *textPage = nullptr;
    char *text = nullptr;
    fz_var(page);
    fz_var(textPage);
    fz_var(text);
    fz_try(m_ctx)
    {
        page = fz_load_page(m_ctx, &m_doc->super, pageIndex);
        textPage = fz_new_stext_page_from_page(m_ctx, page, nullptr);
        fz_point a = fz_make_point(float(start.x()), float(start.y()));
        fz_point b = fz_make_point(float(end.x()), float(end.y()));
        if (snapToWords)
            fz_snap_selection(m_ctx, textPage, &a, &b, FZ_SELECT_WORDS);
        text = fz_copy_selection(m_ctx, textPage, a, b, 0);

        constexpr int kMaxSelectionQuads = 65536;
        std::vector<fz_quad> quads(kMaxSelectionQuads);
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
        fz_drop_stext_page(m_ctx, textPage);
        fz_drop_page(m_ctx, page);
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

    switch (encryption) {
    case Encryption::Keep:
        break;
    case Encryption::Remove:
        opts.do_encrypt = PDF_ENCRYPT_NONE;
        break;
    case Encryption::Aes256:
        if (passUtf8.isEmpty() || passUtf8.size() >= int(sizeof(opts.upwd_utf8))) {
            m_error = "The password must be between 1 and 127 characters.";
            return false;
        }
        opts.do_encrypt = PDF_ENCRYPT_AES_256;
        opts.permissions = -1;
        std::memset(opts.upwd_utf8, 0, sizeof(opts.upwd_utf8));
        std::memset(opts.opwd_utf8, 0, sizeof(opts.opwd_utf8));
        std::memcpy(opts.upwd_utf8, passUtf8.constData(), size_t(passUtf8.size()));
        std::memcpy(opts.opwd_utf8, passUtf8.constData(), size_t(passUtf8.size()));
        break;
    }

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
    for (int pageIndex = 0; pageIndex < count; ++pageIndex) {
        if (progress && !progress(pageIndex, count)) {
            m_error = "Highlighting was cancelled.";
            return false;
        }

        pdf_page *page = nullptr;
        int hits = 0;
        fz_var(page);
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
                pdf_annot *annot = pdf_create_annot(m_ctx, page, PDF_ANNOT_HIGHLIGHT);
                const float yellow[3] = { 1.0f, 0.92f, 0.0f };
                pdf_set_annot_color(m_ctx, annot, 3, yellow);
                pdf_set_annot_quad_points(m_ctx, annot, hits, quads.data());
                pdf_update_annot(m_ctx, annot);
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

        if (hits > 0 && changedPages)
            changedPages->append(pageIndex);
    }

    return true;
}

bool PdfEditor::bakeAnnotations()
{
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }

    fz_try(m_ctx)
    {
        pdf_bake_document(m_ctx, m_doc, 1, 0);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }
    return true;
}

bool PdfEditor::hasAnnotations()
{
    if (!m_doc)
        return false;

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

    for (int i = 0; i < count; ++i) {
        pdf_page *page = nullptr;
        bool found = false;
        fz_var(page);
        fz_var(found);

        fz_try(m_ctx)
        {
            page = pdf_load_page(m_ctx, m_doc, i);
            found = (pdf_first_annot(m_ctx, page) != nullptr);
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

        if (found)
            return true;
    }
    return false;
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
                                     const QString &oldText, const QString &newText)
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

    pdf_page *page = nullptr;
    fz_var(page);

    fz_try(m_ctx)
    {
        page = pdf_load_page(m_ctx, m_doc, pageIndex);

        pdf_annot *annot = pdf_create_annot(m_ctx, page, PDF_ANNOT_INK);
        pdf_set_annot_color(m_ctx, annot, 3, color);
        pdf_set_annot_border(m_ctx, annot, float(width));
        pdf_set_annot_opacity(m_ctx, annot, float(opacity));
        pdf_add_annot_ink_list(m_ctx, annot, count, stroke.data());
        pdf_update_annot(m_ctx, annot);
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
    pdf_page *page = nullptr;
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
                InkStroke stroke;
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

    pdf_page *page = nullptr;
    fz_buffer *buffer = nullptr;
    fz_image *image = nullptr;
    fz_var(page);
    fz_var(buffer);
    fz_var(image);

    fz_try(m_ctx)
    {
        buffer = fz_new_buffer_from_copied_data(m_ctx, data, size);
        image = fz_new_image_from_buffer(m_ctx, buffer);
        page = pdf_load_page(m_ctx, m_doc, pageIndex);

        pdf_annot *annot = pdf_create_annot(m_ctx, page, PDF_ANNOT_STAMP);
        pdf_set_annot_rect(m_ctx, annot, rect);
        pdf_set_annot_stamp_image(m_ctx, annot, image);
        pdf_set_annot_rect(m_ctx, annot, rect);
        pdf_update_annot(m_ctx, annot);
    }
    fz_always(m_ctx)
    {
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
    fz_var(page);
    fz_var(fieldName);
    fz_var(succeeded);

    fz_try(m_ctx)
    {
        page = pdf_load_page(m_ctx, m_doc, pageIndex);
        int widgetIndex = 0;
        for (pdf_annot *widget = pdf_first_widget(m_ctx, page); widget;
             widget = pdf_next_widget(m_ctx, widget), ++widgetIndex) {
            FormField field;
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

bool PdfEditor::setFormFieldValue(int pageIndex, int widgetIndex, const QString &value)
{
    m_error.clear();
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }
    if (widgetIndex < 0) {
        m_error = "That form field could not be found.";
        return false;
    }

    const QByteArray valueUtf8 = value.toUtf8();
    pdf_page *page = nullptr;
    int found = 0;
    int accepted = 0;
    fz_var(page);
    fz_var(found);
    fz_var(accepted);

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
    return true;
}

// background page drawing (see MuPdfRenderCache.h)

namespace {
// Each render thread has its own MuPDF context, so cap its internal cache.
constexpr size_t kRenderStoreBytes = 32u * 1024u * 1024u;
}

MuPdfRenderCache::~MuPdfRenderCache()
{
    closeDocument();
    if (m_ctx)
        fz_drop_context(m_ctx);
}

void MuPdfRenderCache::closeDocument()
{
    if (m_doc) {
        pdf_drop_document(m_ctx, m_doc);
        m_doc = nullptr;
    }
    // The document reads from these bytes, so they go only after it is gone.
    m_bytes.reset();
    m_path.clear();
    m_size = -1;
}

bool MuPdfRenderCache::open(const QString &path, const QString &password,
                            QString *error)
{
    const QFileInfo info(path);
    const QDateTime modified = info.lastModified();
    const qint64 size = info.size();
    if (m_doc && path == m_path && password == m_password
        && modified == m_modified && size == m_size)
        return true;

    closeDocument();

    if (!m_ctx)
        m_ctx = fz_new_context(nullptr, nullptr, kRenderStoreBytes);
    if (!m_ctx) {
        *error = QStringLiteral("Could not start the PDF engine for background rendering.");
        return false;
    }

    m_bytes = RenderDocumentBytesCache::sharedBytes(path, modified, size, error);
    if (!m_bytes)
        return false;

    const QByteArray passUtf8 = password.toUtf8();
    const char *cPass = passUtf8.constData();
    const unsigned char *data =
        reinterpret_cast<const unsigned char *>(m_bytes->constData());
    const size_t length = size_t(m_bytes->size());

    fz_stream *stream = nullptr;
    pdf_document *doc = nullptr;
    fz_var(stream);
    fz_var(doc);

    fz_try(m_ctx)
    {
        stream = fz_open_memory(m_ctx, data, length);
        doc = pdf_open_document_with_stream(m_ctx, stream);
        if (fz_needs_password(m_ctx, &doc->super)
            && !fz_authenticate_password(m_ctx, &doc->super, cPass))
            fz_throw(m_ctx, FZ_ERROR_GENERIC, "Incorrect or missing password");
    }
    fz_always(m_ctx)
    {
        // The document keeps its own reference to the stream.
        if (stream)
            fz_drop_stream(m_ctx, stream);
    }
    fz_catch(m_ctx)
    {
        *error = QString::fromUtf8(fz_caught_message(m_ctx));
        if (doc)
            pdf_drop_document(m_ctx, doc);
        m_bytes.reset();
        return false;
    }

    m_doc = doc;
    m_path = path;
    m_password = password;
    m_modified = modified;
    m_size = size;
    return true;
}

QSizeF MuPdfRenderCache::pagePointSize(int page, QString *error)
{
    if (!m_doc || !m_ctx) {
        *error = QStringLiteral("No document is open for background rendering.");
        return QSizeF();
    }

    fz_page *mupdfPage = nullptr;
    fz_var(mupdfPage);
    float width = 0.0f;
    float height = 0.0f;
    fz_var(width);
    fz_var(height);

    fz_try(m_ctx)
    {
        mupdfPage = fz_load_page(m_ctx, &m_doc->super, page);
        const fz_rect bounds = fz_bound_page(m_ctx, mupdfPage);
        width = bounds.x1 - bounds.x0;
        height = bounds.y1 - bounds.y0;
    }
    fz_always(m_ctx)
    {
        if (mupdfPage)
            fz_drop_page(m_ctx, mupdfPage);
    }
    fz_catch(m_ctx)
    {
        *error = QString::fromUtf8(fz_caught_message(m_ctx));
        return QSizeF();
    }
    return QSizeF(width, height);
}

QImage MuPdfRenderCache::render(int page, const QSize &size, bool annotations,
                                QString *error)
{
    if (!m_doc || !m_ctx) {
        *error = QStringLiteral("No document is open for background rendering.");
        return QImage();
    }
    if (size.width() <= 0 || size.height() <= 0) {
        *error = QStringLiteral("The requested page size is empty.");
        return QImage();
    }

    const int width = size.width();
    const int height = size.height();

    fz_page *mupdfPage = nullptr;
    fz_pixmap *pixmap = nullptr;
    fz_device *device = nullptr;
    fz_var(mupdfPage);
    fz_var(pixmap);
    fz_var(device);

    fz_try(m_ctx)
    {
        mupdfPage = fz_load_page(m_ctx, &m_doc->super, page);
        const fz_rect bounds = fz_bound_page(m_ctx, mupdfPage);
        const float pageWidth = bounds.x1 - bounds.x0;
        const float pageHeight = bounds.y1 - bounds.y0;
        if (pageWidth <= 0.0f || pageHeight <= 0.0f)
            fz_throw(m_ctx, FZ_ERROR_GENERIC, "Page has no size.");

        fz_matrix transform = fz_translate(-bounds.x0, -bounds.y0);
        transform = fz_post_scale(transform, width / pageWidth, height / pageHeight);

        pixmap = fz_new_pixmap_with_bbox(m_ctx, fz_device_bgr(m_ctx),
                                         fz_make_irect(0, 0, width, height),
                                         nullptr, 0);
        fz_clear_pixmap_with_value(m_ctx, pixmap, 0xff);

        device = fz_new_draw_device(m_ctx, fz_identity, pixmap);
        if (annotations)
            fz_run_page(m_ctx, mupdfPage, device, transform, nullptr);
        else
            fz_run_page_contents(m_ctx, mupdfPage, device, transform, nullptr);
        fz_close_device(m_ctx, device);
    }
    fz_always(m_ctx)
    {
        if (device)
            fz_drop_device(m_ctx, device);
        if (mupdfPage)
            fz_drop_page(m_ctx, mupdfPage);
    }
    fz_catch(m_ctx)
    {
        if (pixmap)
            fz_drop_pixmap(m_ctx, pixmap);
        *error = QString::fromUtf8(fz_caught_message(m_ctx));
        return QImage();
    }

    QImage wrapper(fz_pixmap_samples(m_ctx, pixmap), width, height,
                   int(fz_pixmap_stride(m_ctx, pixmap)), QImage::Format_BGR888);
    QImage image = wrapper.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    fz_drop_pixmap(m_ctx, pixmap);
    return image;
}