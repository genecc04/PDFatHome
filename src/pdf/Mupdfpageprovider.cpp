#include "MuPdfPageProvider.h"
#include "RenderDocumentBytesCache.h"

#include "reconstruction/AppearanceEstimator.h"
#include "reconstruction/FontClassifier.h"
#include "reconstruction/ParagraphGrouper.h"

#include <QFileInfo>
#include <QtGlobal>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

namespace {

constexpr size_t kStoreBytes = 32u * 1024u * 1024u;

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

QString cleanFontName(const char *rawName)
{
    QString name = QString::fromUtf8(rawName ? rawName : "");

    if (name.size() > 7 && name.at(6) == QLatin1Char('+')) {
        bool isTag = true;
        for (int i = 0; i < 6; ++i) {
            const QChar c = name.at(i);
            if (c < QLatin1Char('A') || c > QLatin1Char('Z'))
                isTag = false;
        }

        if (isTag)
            name.remove(0, 7);
    }

    return name;
}

}

MuPdfPageProvider::~MuPdfPageProvider()
{
    closeDocument();
    if (m_ctx)
        fz_drop_context(m_ctx);
}

void MuPdfPageProvider::closeDocument()
{
    if (m_doc) {
        pdf_drop_document(m_ctx, m_doc);
        m_doc = nullptr;
    }
    m_bytes.reset();
    m_source = DocumentSource();
    m_size = -1;
}

bool MuPdfPageProvider::open(const DocumentSource &source, QString *error)
{
    QString localError;
    QString &message = error ? *error : localError;
    message.clear();

    const QFileInfo info(source.path);
    const QDateTime modified = info.lastModified();
    const qint64 size = info.size();
    if (m_doc && source.path == m_source.path && source.password == m_source.password
        && modified == m_modified && size == m_size)
        return true;

    closeDocument();

    if (!m_ctx)
        m_ctx = fz_new_context(nullptr, nullptr, kStoreBytes);
    if (!m_ctx) {
        message = QStringLiteral("Could not start the PDF engine.");
        return false;
    }

    m_bytes = RenderDocumentBytesCache::sharedBytes(source.path, modified, size, &message);
    if (!m_bytes)
        return false;

    const QByteArray passUtf8 = source.password.toUtf8();
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
        if (stream)
            fz_drop_stream(m_ctx, stream);
    }
    fz_catch(m_ctx)
    {
        message = QString::fromUtf8(fz_caught_message(m_ctx));
        if (doc)
            pdf_drop_document(m_ctx, doc);
        m_bytes.reset();
        return false;
    }

    m_doc = doc;
    m_source = source;
    m_modified = modified;
    m_size = size;
    return true;
}

QSizeF MuPdfPageProvider::pageSize(int pageIndex, QString *error)
{
    if (error)
        error->clear();
    if (!m_doc || !m_ctx) {
        setError(error, QStringLiteral("No document is open."));
        return {};
    }

    fz_page *page = nullptr;
    float width = 0.0f;
    float height = 0.0f;
    fz_var(page);

    fz_try(m_ctx)
    {
        page = fz_load_page(m_ctx, &m_doc->super, pageIndex);
        const fz_rect bounds = fz_bound_page(m_ctx, page);
        width = bounds.x1 - bounds.x0;
        height = bounds.y1 - bounds.y0;
    }
    fz_always(m_ctx)
    {
        if (page)
            fz_drop_page(m_ctx, page);
    }
    fz_catch(m_ctx)
    {
        setError(error, QString::fromUtf8(fz_caught_message(m_ctx)));
        return {};
    }

    if (width <= 0.0f || height <= 0.0f) {
        setError(error, QStringLiteral("Page has no size."));
        return {};
    }
    return QSizeF(width, height);
}

QImage MuPdfPageProvider::renderPage(int pageIndex, const QSize &maximumSize, QString *error)
{
    if (error)
        error->clear();
    if (!m_doc || !m_ctx) {
        setError(error, QStringLiteral("No document is open."));
        return QImage();
    }
    if (maximumSize.width() <= 0 || maximumSize.height() <= 0) {
        setError(error, QStringLiteral("The requested page size is empty."));
        return QImage();
    }

    QImage image;
    fz_page *page = nullptr;
    fz_pixmap *pixmap = nullptr;
    fz_device *device = nullptr;
    int width = 0;
    int height = 0;
    fz_var(page);
    fz_var(pixmap);
    fz_var(device);
    fz_var(image);
    fz_var(width);
    fz_var(height);

    fz_try(m_ctx)
    {
        page = fz_load_page(m_ctx, &m_doc->super, pageIndex);
        const fz_rect bounds = fz_bound_page(m_ctx, page);
        const float pageWidth = bounds.x1 - bounds.x0;
        const float pageHeight = bounds.y1 - bounds.y0;
        if (pageWidth <= 0.0f || pageHeight <= 0.0f)
            fz_throw(m_ctx, FZ_ERROR_GENERIC, "Page has no size.");

        const float scale = qMin(float(maximumSize.width()) / pageWidth,
                                 float(maximumSize.height()) / pageHeight);
        width = qMax(1, qRound(pageWidth * scale));
        height = qMax(1, qRound(pageHeight * scale));

        fz_matrix transform = fz_translate(-bounds.x0, -bounds.y0);
        transform = fz_post_scale(transform, width / pageWidth, height / pageHeight);

        image = QImage(width, height, QImage::Format_RGB32);
        if (image.isNull())
            fz_throw(m_ctx, FZ_ERROR_GENERIC, "Could not allocate the page image.");

        pixmap = fz_new_pixmap_with_data(m_ctx, fz_device_bgr(m_ctx), width, height, nullptr, 1,
                                         int(image.bytesPerLine()), image.bits());
        fz_clear_pixmap_with_value(m_ctx, pixmap, 0xff);

        device = fz_new_draw_device(m_ctx, fz_identity, pixmap);
        fz_run_page(m_ctx, page, device, transform, nullptr);
        fz_close_device(m_ctx, device);
    }
    fz_always(m_ctx)
    {
        if (device)
            fz_drop_device(m_ctx, device);
        if (page)
            fz_drop_page(m_ctx, page);
        if (pixmap)
            fz_drop_pixmap(m_ctx, pixmap);
    }
    fz_catch(m_ctx)
    {
        setError(error, QString::fromUtf8(fz_caught_message(m_ctx)));
        return QImage();
    }

    return image;
}

Reconstruction::ReconstructedPage MuPdfPageProvider::extractTextLayer(int pageIndex,
                                                                      QString *error)
{
    using namespace Reconstruction;

    if (error)
        error->clear();
    if (!m_doc || !m_ctx) {
        setError(error, QStringLiteral("No document is open."));
        return {};
    }

    ReconstructedPage result;
    fz_page *page = nullptr;
    fz_stext_page *stext = nullptr;
    float originX = 0.0f;
    float originY = 0.0f;
    fz_var(page);
    fz_var(stext);

    fz_try(m_ctx)
    {
        page = fz_load_page(m_ctx, &m_doc->super, pageIndex);
        const fz_rect bounds = fz_bound_page(m_ctx, page);
        originX = bounds.x0;
        originY = bounds.y0;
        result.pageIndex = pageIndex;
        result.pageSize = QSizeF(bounds.x1 - bounds.x0, bounds.y1 - bounds.y0);

        fz_stext_options options = {};
        stext = fz_new_stext_page_from_page(m_ctx, page, &options);
    }
    fz_always(m_ctx)
    {
        if (page)
            fz_drop_page(m_ctx, page);
    }
    fz_catch(m_ctx)
    {
        if (stext)
            fz_drop_stext_page(m_ctx, stext);
        setError(error, QString::fromUtf8(fz_caught_message(m_ctx)));
        return {};
    }

    auto toRect = [originX, originY](const fz_rect &r) {
        return QRectF(r.x0 - originX, r.y0 - originY, r.x1 - r.x0, r.y1 - r.y0);
    };

    for (fz_stext_block *b = stext->first_block; b; b = b->next) {
        if (b->type != FZ_STEXT_BLOCK_TEXT)
            continue;

        TextBlock block;
        block.bounds = toRect(b->bbox);

        for (fz_stext_line *l = b->u.t.first_line; l; l = l->next) {
            if (!l->first_char)
                continue;

            TextLine line;
            line.bounds = toRect(l->bbox);

            const fz_stext_char *first = l->first_char;
            const fz_stext_char *last = l->last_char;
            line.baseline = QLineF(
                first->origin.x - originX,
                first->origin.y - originY,
                last->origin.x - originX + (last->quad.lr.x - last->quad.ll.x),
                last->origin.y - originY + (last->quad.lr.y - last->quad.ll.y));

            QString text;
            QRectF wordBounds;
            bool haveBounds = false;
            float sizeSum = 0.0f;
            int charCount = 0;
            QString fontName;
            bool bold = false;
            bool italic = false;
            FontFamily family = FontFamily::Unknown;

            auto flushWord = [&]() {
                if (!text.isEmpty()) {
                    TextWord word;
                    word.text = text;
                    word.bounds = wordBounds;
                    word.confidence = 100.0f;
                    word.fontSize = sizeSum / float(charCount);
                    word.fontName = fontName;
                    word.bold = bold;
                    word.italic = italic;
                    word.family = family;
                    line.words.append(word);
                }

                text.clear();
                wordBounds = QRectF();
                haveBounds = false;
                sizeSum = 0.0f;
                charCount = 0;
                fontName.clear();
                bold = false;
                italic = false;
                family = FontFamily::Unknown;
            };

            for (const fz_stext_char *ch = l->first_char; ch; ch = ch->next) {
                const uint codePoint = uint(ch->c);

                if (QChar::isSpace(codePoint)) {
                    flushWord();
                    continue;
                }

                if (text.isEmpty() && ch->font) {
                    fontName = cleanFontName(fz_font_name(m_ctx, ch->font));

                    const FontStyle style = mergeFontEvidence(
                        fontName,
                        fz_font_is_bold(m_ctx, ch->font) != 0,
                        fz_font_is_italic(m_ctx, ch->font) != 0,
                        false,
                        fz_font_is_monospaced(m_ctx, ch->font) != 0);
                    bold = style.bold;
                    italic = style.italic;
                    family = style.family;
                }

                const char32_t cp = char32_t(codePoint);
                text += QString::fromUcs4(&cp, 1);

                const QRectF charRect = toRect(fz_rect_from_quad(ch->quad));
                wordBounds = haveBounds ? wordBounds.united(charRect) : charRect;
                haveBounds = true;

                sizeSum += ch->size;
                ++charCount;
            }

            flushWord();

            if (!line.words.isEmpty())
                block.lines.append(line);
        }

        if (!block.lines.isEmpty())
            result.blocks.append(block);
    }

    fz_drop_stext_page(m_ctx, stext);

    if (result.blocks.isEmpty())
        return result;

    splitLinesAtGaps(result);

    QString renderError;
    const QImage render = renderPage(pageIndex, QSize(1275, 1650), &renderError);
    if (!render.isNull())
        estimateLineStyles(result, render);

    ParagraphOptions options;
    options.respectBlocks = false;

    options.maxSizeChange = 0.06;
    groupParagraphs(result, options);

    if (!render.isNull())
        estimateAppearance(result, render);

    return result;
}