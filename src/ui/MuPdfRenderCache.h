#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QImage>
#include <QRectF>
#include <QSize>
#include <QSizeF>
#include <QString>

#include <memory>

struct fz_context;
struct pdf_document;

class MuPdfRenderCache
{
public:
    MuPdfRenderCache() = default;
    ~MuPdfRenderCache();
    MuPdfRenderCache(const MuPdfRenderCache &) = delete;
    MuPdfRenderCache &operator=(const MuPdfRenderCache &) = delete;

    bool open(const QString &path, const QString &password, QString *error);
    QSizeF pagePointSize(int page, QString *error);
    QImage render(int page, const QSize &size, bool annotations, QString *error);
    QImage renderRegion(int page, const QRectF &pageRegion, const QSize &size,
                       bool annotations, QString *error);

private:
    void closeDocument();

    fz_context *m_ctx = nullptr;
    pdf_document *m_doc = nullptr;
    std::shared_ptr<QByteArray> m_bytes;
    QString m_path;
    QString m_password;
    QDateTime m_modified;
    qint64 m_size = -1;
};
