#include "reconstruction/AutoReconstruct.h"

#include <QChar>
#include <QList>
#include <QtGlobal>

namespace Reconstruction {

namespace {

bool isReadable(uint ch)
{
    if (ch == 0xFFFD)                       // replacement character
        return false;

    return QChar::isLetterOrNumber(ch)
        || QChar::isPunct(ch)
        || QChar::isSymbol(ch);
}

}

bool hasUsableText(const ReconstructedPage &page,
                   int minCharacters,
                   double minReadableRatio)
{
    int total = 0;
    int readable = 0;

    for (const TextBlock &block : page.blocks) {
        for (const TextLine &line : block.lines) {
            for (const TextWord &word : line.words) {
                const QList<uint> chars = word.text.toUcs4();
                for (const uint ch : chars) {
                    ++total;
                    if (isReadable(ch))
                        ++readable;
                }
            }
        }
    }

    if (total < minCharacters)
        return false;

    return double(readable) / double(total) >= minReadableRatio;
}

AutoResult reconstructPageAuto(PageProvider &provider,
                               ReconstructionEngine &engine,
                               int pageIndex,
                               const AutoOptions &options)
{
    AutoResult result;
    QString error;

    // 1) Text layer: exact characters, fonts and sizes, and fast.
    ReconstructedPage text = provider.extractTextLayer(pageIndex, &error);
    if (hasUsableText(text, options.minCharacters, options.minReadableRatio)) {
        result.page = std::move(text);
        result.source = PageSource::TextLayer;
        result.ok = true;
        return result;
    }

    // Keep what the text layer gave us, in case OCR can't run.
    const bool haveWeakText = !text.paragraphs.isEmpty();

    auto fallBack = [&](const QString &why) {
        if (haveWeakText) {
            result.page = std::move(text);
            result.source = PageSource::TextLayer;
            result.ok = true;
            result.note = QStringLiteral("%1 Showing the page's own text, which may be incomplete.")
                              .arg(why);
        } else {
            result.ok = false;
            result.note = why;
        }
        return result;
    };

    // 2) No usable text: OCR the page.
    const QSizeF pageSize = provider.pageSize(pageIndex, &error);
    if (pageSize.isEmpty())
        return fallBack(error.isEmpty() ? QStringLiteral("Could not read the page size.") : error);

    if (!engine.isInitialized()) {
        QString initError;
        if (!engine.initialize(options.tessdataPath, options.language, &initError))
            return fallBack(initError);
    }

    // Aim for ocrDpi, but never exceed the size cap.
    double scale = options.ocrDpi / 72.0;
    const double longest = qMax(pageSize.width(), pageSize.height()) * scale;
    if (longest > options.maxOcrSide)
        scale *= options.maxOcrSide / longest;

    const QSize renderSize(qMax(1, qRound(pageSize.width() * scale)),
                           qMax(1, qRound(pageSize.height() * scale)));

    const QImage image = provider.renderPage(pageIndex, renderSize, &error);
    if (image.isNull())
        return fallBack(error.isEmpty() ? QStringLiteral("Could not render the page for OCR.") : error);

    result.page = engine.reconstructPage(image, pageIndex, pageSize);
    result.source = PageSource::Ocr;
    result.ok = true;

    if (result.page.paragraphs.isEmpty())
        result.note = QStringLiteral("OCR found no text on this page.");

    return result;
}

}