#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QImage>
#include <QRectF>
#include <QSize>
#include <QSizeF>
#include <QString>

#include <memory>

#include "DocumentSource.h"

struct fz_context;
struct pdf_document;

// A per-thread MuPDF document for drawing pages in the background.
class MuPdfRenderCache
{
public:
    MuPdfRenderCache() = default;
    ~MuPdfRenderCache();
    MuPdfRenderCache(const MuPdfRenderCache &) = delete;
    MuPdfRenderCache &operator=(const MuPdfRenderCache &) = delete;

    bool open(const DocumentSource &source, QString *error);
    QImage render(int page, const QSize &size, bool annotations, QString *error);
    QImage renderThumbnail(int page, const QSizeF &maxSize, QString *error);
    QImage renderRegion(int page, const QRectF &pageRegion, const QSize &size,
                       bool annotations, QString *error);

private:
    QImage renderRect(int page, const QRectF *region, const QSize &requestedSize,
                      const QSizeF &fitWithin, bool annotations, QString *error);
    void closeDocument();

    fz_context *m_ctx = nullptr;
    pdf_document *m_doc = nullptr;
    std::shared_ptr<QByteArray> m_bytes;
    DocumentSource m_source;
    QDateTime m_modified;
    qint64 m_size = -1;
};
