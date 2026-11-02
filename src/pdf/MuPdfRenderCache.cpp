#include "MuPdfRenderCache.h"
#include "RenderDocumentBytesCache.h"

#include <QFileInfo>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

namespace {
// Each render thread has its own MuPDF context, so cap its internal cache.
constexpr size_t kRenderStoreBytes = 32u * 1024u * 1024u;

// MuPDF draws BGRA bytes straight into the QImage; that matches the in-memory
// layout of Format_RGB32 on little-endian. The page is cleared to opaque white.
fz_pixmap *newPixmapOverImage(fz_context *ctx, QImage &image, int width, int height)
{
    image = QImage(width, height, QImage::Format_RGB32);
    if (image.isNull())
        fz_throw(ctx, FZ_ERROR_GENERIC, "Could not allocate the page image.");
    return fz_new_pixmap_with_data(ctx, fz_device_bgr(ctx), width, height, nullptr, 1,
                                   int(image.bytesPerLine()), image.bits());
}
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
    m_source = DocumentSource();
    m_size = -1;
}

bool MuPdfRenderCache::open(const DocumentSource &source, QString *error)
{
    const QFileInfo info(source.path);
    const QDateTime modified = info.lastModified();
    const qint64 size = info.size();
    if (m_doc && source.path == m_source.path && source.password == m_source.password
        && modified == m_modified && size == m_size)
        return true;

    closeDocument();

    if (!m_ctx)
        m_ctx = fz_new_context(nullptr, nullptr, kRenderStoreBytes);
    if (!m_ctx) {
        *error = QStringLiteral("Could not start the PDF engine for background rendering.");
        return false;
    }

    m_bytes = RenderDocumentBytesCache::sharedBytes(source.path, modified, size, error);
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
    m_source = source;
    m_modified = modified;
    m_size = size;
    return true;
}

QImage MuPdfRenderCache::render(int page, const QSize &size, bool annotations,
                                QString *error)
{
    if (size.width() <= 0 || size.height() <= 0) {
        *error = QStringLiteral("The requested page size is empty.");
        return QImage();
    }
    return renderRect(page, nullptr, size, QSizeF(), annotations, error);
}

QImage MuPdfRenderCache::renderThumbnail(int page, const QSizeF &maxSize,
                                         QString *error)
{
    if (maxSize.width() <= 0.0 || maxSize.height() <= 0.0) {
        *error = QStringLiteral("The requested page size is empty.");
        return QImage();
    }
    return renderRect(page, nullptr, QSize(), maxSize, true, error);
}

QImage MuPdfRenderCache::renderRegion(int page, const QRectF &pageRegion,
                                      const QSize &size, bool annotations,
                                      QString *error)
{
    if (size.width() <= 0 || size.height() <= 0
        || pageRegion.width() <= 0.0 || pageRegion.height() <= 0.0) {
        *error = QStringLiteral("The requested page region or output size is empty.");
        return QImage();
    }
    return renderRect(page, &pageRegion, size, QSizeF(), annotations, error);
}

// Draws the page, or the part of it in region (page points, relative to the
// page's top-left), into a new image. Without a region and with an empty
// requestedSize, the page is scaled to fit within fitWithin.
QImage MuPdfRenderCache::renderRect(int page, const QRectF *region,
                                    const QSize &requestedSize, const QSizeF &fitWithin,
                                    bool annotations, QString *error)
{
    if (!m_doc || !m_ctx) {
        *error = QStringLiteral("No document is open for background rendering.");
        return QImage();
    }

    int width = requestedSize.width();
    int height = requestedSize.height();

    QImage image;
    fz_page *mupdfPage = nullptr;
    fz_pixmap *pixmap = nullptr;
    fz_device *device = nullptr;
    fz_var(mupdfPage);
    fz_var(pixmap);
    fz_var(device);
    fz_var(image);
    fz_var(width);
    fz_var(height);

    fz_try(m_ctx)
    {
        mupdfPage = fz_load_page(m_ctx, &m_doc->super, page);
        const fz_rect bounds = fz_bound_page(m_ctx, mupdfPage);
        const float pageWidth = bounds.x1 - bounds.x0;
        const float pageHeight = bounds.y1 - bounds.y0;
        if (pageWidth <= 0.0f || pageHeight <= 0.0f)
            fz_throw(m_ctx, FZ_ERROR_GENERIC, "Page has no size.");

        // The part of the page that fills the whole image.
        fz_rect source = bounds;
        if (region) {
            source.x0 = bounds.x0 + float(region->left());
            source.y0 = bounds.y0 + float(region->top());
            source.x1 = qMin(bounds.x1, bounds.x0 + float(region->right()));
            source.y1 = qMin(bounds.y1, bounds.y0 + float(region->bottom()));
            if (source.x1 <= source.x0 || source.y1 <= source.y0)
                fz_throw(m_ctx, FZ_ERROR_GENERIC,
                         "The requested page region is outside the page.");
        } else if (width <= 0 || height <= 0) {
            const qreal scale = qMin(fitWithin.width() / pageWidth,
                                     fitWithin.height() / pageHeight);
            width = qMax(1, int(pageWidth * scale));
            height = qMax(1, int(pageHeight * scale));
        }
        fz_matrix transform = fz_translate(-source.x0, -source.y0);
        transform = fz_post_scale(transform, width / (source.x1 - source.x0),
                                  height / (source.y1 - source.y0));

        pixmap = newPixmapOverImage(m_ctx, image, width, height);
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
        if (pixmap)
            fz_drop_pixmap(m_ctx, pixmap);
    }
    fz_catch(m_ctx)
    {
        *error = QString::fromUtf8(fz_caught_message(m_ctx));
        return QImage();
    }

    return image;
}
