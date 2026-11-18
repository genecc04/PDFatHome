#pragma once

#include "ReconstructedPage.h"
#include "ReconstructionEngine.h"

#include <QImage>
#include <QSize>
#include <QSizeF>
#include <QString>

namespace Reconstruction {

// What the host app implements; the module knows nothing about the PDF library.
class PageProvider
{
public:
    virtual ~PageProvider() = default;

    // Page size in points (crop box, after rotation).
    virtual QSizeF pageSize(int pageIndex, QString *error) = 0;

    // The page's text layer, fully built (blocks, lines, words, paragraphs).
    // A page with no text layer may come back with no words.
    virtual ReconstructedPage extractTextLayer(int pageIndex, QString *error) = 0;

    // A render that fits inside maximumSize, keeping the page's proportions.
    virtual QImage renderPage(int pageIndex, const QSize &maximumSize, QString *error) = 0;
};

enum class PageSource { None, TextLayer, Ocr };

struct AutoOptions
{
    // Fewer characters than this counts as "no text layer".
    int minCharacters = 20;

    // Share of characters that must look like real text (letters, digits,
    // punctuation, symbols). A layer full of replacement or private-use
    // characters (broken font map, bad hidden OCR) is rejected in favor of OCR.
    double minReadableRatio = 0.85;

    // OCR render resolution, and a cap on the longest side in pixels.
    int ocrDpi = 300;
    int maxOcrSide = 6000;

    // Used only if OCR is needed; the engine is initialized on first use.
    QString tessdataPath;
    QString language = QStringLiteral("eng");
};

struct AutoResult
{
    ReconstructedPage page;
    PageSource source = PageSource::None;
    bool ok = false;      // false: nothing could be produced; see `note`
    QString note;         // explanation or warning to show the user
};

// True if the page has enough real text to trust the text layer.
bool hasUsableText(const ReconstructedPage &page,
                   int minCharacters = 20,
                   double minReadableRatio = 0.85);

// Text layer first; OCR only if the page has no usable text.
AutoResult reconstructPageAuto(PageProvider &provider,
                               ReconstructionEngine &engine,
                               int pageIndex,
                               const AutoOptions &options = AutoOptions());

}