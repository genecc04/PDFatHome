#include "reconstruction/ReconstructionEngine.h"
#include "reconstruction/AppearanceEstimator.h"
#include "reconstruction/FontClassifier.h"
#include "reconstruction/ParagraphGrouper.h"

#include <QByteArray>
#include <QDir>
#include <QFile>

#include <tesseract/baseapi.h>
#include <tesseract/resultiterator.h>

#include <memory>

namespace Reconstruction {

struct ReconstructionEngine::Impl
{
    // Declaration order matters: members are destroyed in reverse, and Tesseract
    // must go before the bytes it was loaded from.
    QByteArray traineddata;
    tesseract::TessBaseAPI api;
    bool ready = false;
};

namespace {

// Tesseract boxes are left/top/right/bottom; Qt wants x/y/width/height.
QRectF boxAt(const tesseract::ResultIterator &it, tesseract::PageIteratorLevel level)
{
    int left = 0, top = 0, right = 0, bottom = 0;
    if (!it.BoundingBox(level, &left, &top, &right, &bottom))
        return {};

    return QRectF(left, top, right - left, bottom - top);
}

QLineF baselineAt(const tesseract::ResultIterator &it, tesseract::PageIteratorLevel level)
{
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    if (!it.Baseline(level, &x1, &y1, &x2, &y2))
        return {};

    return QLineF(x1, y1, x2, y2);
}

QRectF scaledRect(const QRectF &r, double sx, double sy)
{
    return QRectF(r.x() * sx, r.y() * sy, r.width() * sx, r.height() * sy);
}

// Image pixels -> page points, for every rectangle in the page.
void scalePageToPoints(ReconstructedPage &page, double sx, double sy)
{
    for (TextBlock &block : page.blocks) {
        block.bounds = scaledRect(block.bounds, sx, sy);

        for (TextLine &line : block.lines) {
            line.bounds = scaledRect(line.bounds, sx, sy);
            line.baseline = QLineF(line.baseline.x1() * sx, line.baseline.y1() * sy,
                                   line.baseline.x2() * sx, line.baseline.y2() * sy);

            for (TextWord &word : line.words)
                word.bounds = scaledRect(word.bounds, sx, sy);
        }
    }
}

}

ReconstructionEngine::ReconstructionEngine()
    : m_impl(std::make_unique<Impl>())
{
}

ReconstructionEngine::~ReconstructionEngine() = default;

bool ReconstructionEngine::initialize(
    const QString &tessdataPath,
    const QString &language,
    QString *error,
    bool wantFontAttributes)
{
    if (error)
        error->clear();

    auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        m_impl->ready = false;
        return false;
    };

    // Read the file with Qt and hand Tesseract the bytes: a folder path goes
    // through a narrow (ANSI) string on Windows and fails on non-ASCII folder names.
    QFile file(QDir(tessdataPath).filePath(language + QStringLiteral(".traineddata")));
    if (!file.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("Could not open %1: %2")
                        .arg(file.fileName(), file.errorString()));

    const QByteArray data = file.readAll();
    if (data.isEmpty())
        return fail(QStringLiteral("%1 is empty.").arg(file.fileName()));

    m_impl->ready = false;
    m_impl->api.End();
    m_impl->traineddata = data;

    const QByteArray lang = language.toUtf8();

    if (m_impl->api.Init(m_impl->traineddata.constData(),
                         int(m_impl->traineddata.size()),
                         lang.constData(),
                         wantFontAttributes ? tesseract::OEM_TESSERACT_LSTM_COMBINED
                                            : tesseract::OEM_LSTM_ONLY,
                         nullptr, 0, nullptr, nullptr, false, nullptr) != 0) {
        return fail(QStringLiteral("Could not load Tesseract language '%1' from %2")
                        .arg(language, file.fileName()));
    }

    m_impl->api.SetPageSegMode(tesseract::PSM_AUTO);
    m_impl->ready = true;
    return true;
}

bool ReconstructionEngine::isInitialized() const
{
    return m_impl->ready;
}

ReconstructedPage ReconstructionEngine::reconstructPage(
    const QImage &image,
    int pageIndex,
    const QSizeF &pageSize)
{
    ReconstructedPage page;

    page.pageIndex = pageIndex;
    page.pageSize = pageSize;
    page.imageSize = image.size();

    if (!m_impl->ready || image.isNull())
        return page;

    // Refuse absurd sizes instead of exhausting memory inside Tesseract.
    constexpr int kMaxSide = 20000;
    if (image.width() > kMaxSide || image.height() > kMaxSide)
        return page;

    const QImage rgb = (image.format() == QImage::Format_RGB888)
        ? image
        : image.convertToFormat(QImage::Format_RGB888);

    auto &api = m_impl->api;

    // SetImage copies the pixels, so `rgb` only needs to live through this call.
    api.SetImage(rgb.constBits(), rgb.width(), rgb.height(),
                 3, int(rgb.bytesPerLine()));

    // The real DPI improves recognition and makes font-size estimates true points.
    if (pageSize.width() > 0) {
        const int dpi = qRound(rgb.width() / pageSize.width() * 72.0);
        if (dpi > 0)
            api.SetSourceResolution(dpi);
    }

    if (api.Recognize(nullptr) != 0) {
        api.Clear();
        return page;
    }

    std::unique_ptr<tesseract::ResultIterator> it(api.GetIterator());

    if (it) {
        do {
            if (it->IsAtBeginningOf(tesseract::RIL_BLOCK)) {
                TextBlock block;
                block.bounds = boxAt(*it, tesseract::RIL_BLOCK);
                page.blocks.append(block);
            }

            if (page.blocks.isEmpty())
                page.blocks.append(TextBlock{});

            if (it->IsAtBeginningOf(tesseract::RIL_TEXTLINE)) {
                TextLine line;
                line.bounds = boxAt(*it, tesseract::RIL_TEXTLINE);
                line.baseline = baselineAt(*it, tesseract::RIL_TEXTLINE);
                page.blocks.last().lines.append(line);
            }

            if (page.blocks.last().lines.isEmpty())
                page.blocks.last().lines.append(TextLine{});

            std::unique_ptr<char[]> utf8(it->GetUTF8Text(tesseract::RIL_WORD));
            if (!utf8)
                continue;

            const QString text = QString::fromUtf8(utf8.get()).trimmed();
            if (text.isEmpty())
                continue;

            TextWord word;
            word.text = text;
            word.bounds = boxAt(*it, tesseract::RIL_WORD);
            word.confidence = it->Confidence(tesseract::RIL_WORD); // 0..100

            // Best-guess attributes; pointSize stays 0 if unknown.
            bool bold = false, italic = false, underlined = false;
            bool monospace = false, serif = false, smallcaps = false;
            int pointSize = 0, fontId = -1;
            const char *tesseractFont = it->WordFontAttributes(
                &bold, &italic, &underlined, &monospace,
                &serif, &smallcaps, &pointSize, &fontId);
            word.fontSize = float(pointSize);

            // A null name means no font data (always so in LSTM-only mode); the
            // flags are then just defaults, so don't trust them. word.fontName
            // stays empty on purpose: empty marks an OCR word everywhere else.
            if (tesseractFont) {
                const FontStyle guess = mergeFontEvidence(
                    QString::fromUtf8(tesseractFont), bold, italic, serif, monospace);
                word.bold = guess.bold;
                word.italic = guess.italic;
                word.family = guess.family;
            }

            page.blocks.last().lines.last().words.append(word);

        } while (it->Next(tesseract::RIL_WORD));
    }

    // The iterator points into the results, so destroy it BEFORE Clear() frees them.
    it.reset();
    api.Clear();

    // Tesseract works in image pixels; convert to page points so the result does
    // not depend on the render resolution. Without a page size, boxes stay in pixels.
    if (pageSize.width() > 0 && pageSize.height() > 0) {
        const double sx = pageSize.width() / rgb.width();
        const double sy = pageSize.height() / rgb.height();
        scalePageToPoints(page, sx, sy);
    }

    const bool haveSize = pageSize.width() > 0 && pageSize.height() > 0;

    // Order matters, and the grouping rules are measured in points: split lines
    // at wide gaps, measure per-line styles, group into paragraphs (which uses those styles), then measure each paragraph's colors.
    splitLinesAtGaps(page);

    if (haveSize)
        estimateLineStyles(page, rgb);

    groupParagraphs(page);

    if (haveSize)
        estimateAppearance(page, rgb);

    return page;
}

}