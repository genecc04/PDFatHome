#pragma once

#include "ReconstructedPage.h"

#include <QImage>
#include <QSizeF>
#include <QString>

#include <memory>

namespace Reconstruction {

// OCR front end (one Tesseract instance). NOT thread-safe: give each worker
// thread its own engine. initialize() is slow (loads language data); call it
// once per engine, not once per page.
class ReconstructionEngine
{
public:
    ReconstructionEngine();
    ~ReconstructionEngine();

    ReconstructionEngine(const ReconstructionEngine &) = delete;
    ReconstructionEngine &operator=(const ReconstructionEngine &) = delete;

    // Loads "<tessdataPath>/<language>.traineddata". `language` is a single
    // language ("eng"), not a combination. Safe to call again.
    // wantFontAttributes: combined legacy+LSTM mode so words get font attributes
    // (default LSTM-only has none).
    bool initialize(const QString &tessdataPath,
                const QString &language,
                QString *error = nullptr,
                bool wantFontAttributes = false);
    bool isInitialized() const;

    // OCRs a render of the whole page. The result is in page points, with
    // paragraphs already grouped and colors estimated.
    ReconstructedPage reconstructPage(
        const QImage &image,
        int pageIndex,
        const QSizeF &pageSize
    );

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}