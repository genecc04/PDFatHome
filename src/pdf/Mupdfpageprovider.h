#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QImage>
#include <QSize>
#include <QSizeF>
#include <QString>

#include <memory>

#include "DocumentSource.h"
#include "reconstruction/AutoReconstruct.h"

struct fz_context;
struct pdf_document;

class MuPdfPageProvider final : public Reconstruction::PageProvider
{
public:
    MuPdfPageProvider() = default;
    ~MuPdfPageProvider() override;
    MuPdfPageProvider(const MuPdfPageProvider &) = delete;
    MuPdfPageProvider &operator=(const MuPdfPageProvider &) = delete;

    bool open(const DocumentSource &source, QString *error);

    QSizeF pageSize(int pageIndex, QString *error) override;
    Reconstruction::ReconstructedPage extractTextLayer(int pageIndex, QString *error) override;
    QImage renderPage(int pageIndex, const QSize &maximumSize, QString *error) override;

private:
    void closeDocument();

    fz_context *m_ctx = nullptr;
    pdf_document *m_doc = nullptr;
    std::shared_ptr<QByteArray> m_bytes;
    DocumentSource m_source;
    QDateTime m_modified;
    qint64 m_size = -1;
};