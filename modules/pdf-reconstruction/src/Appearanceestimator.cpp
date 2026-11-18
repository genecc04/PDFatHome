#include "reconstruction/AppearanceEstimator.h"

#include <QHash>
#include <QVector>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace Reconstruction {

namespace {

// Read-only access to pixels as QRgb, straight from the scanlines.
// QImage::pixelColor() is far too slow for every pixel of a 300 DPI page.
// RGB32/ARGB32/RGB888 are read in place; other formats are converted once.
class Pixels
{
public:
    explicit Pixels(const QImage &source)
        : m_image(isDirect(source.format()) ? source
                                            : source.convertToFormat(QImage::Format_RGB32))
        , m_rgb888(m_image.format() == QImage::Format_RGB888)
    {
    }

    int width() const { return m_image.width(); }
    int height() const { return m_image.height(); }
    QRect rect() const { return QRect(0, 0, width(), height()); }

    // The caller keeps x and y inside rect().
    QRgb at(int x, int y) const
    {
        const uchar *line = m_image.constScanLine(y);
        if (m_rgb888) {
            const uchar *p = line + 3 * x;
            return qRgb(p[0], p[1], p[2]);
        }
        return reinterpret_cast<const QRgb *>(line)[x];
    }

private:
    static bool isDirect(QImage::Format format)
    {
        return format == QImage::Format_RGB32
            || format == QImage::Format_ARGB32
            || format == QImage::Format_RGB888;
    }

    QImage m_image;
    bool m_rgb888;
};

struct Sum
{
    int count = 0;
    long long r = 0;
    long long g = 0;
    long long b = 0;
};

struct RegionStats
{
    QColor background = QColor(Qt::white);
    QColor text = QColor(Qt::black);
    int farthest = 0;         // distance of the most different pixel from the background
    double inkDensity = 0.0;  // fraction of pixels that are clearly ink
    bool valid = false;
};

int distance(QRgb a, QRgb b)
{
    return qAbs(qRed(a) - qRed(b)) + qAbs(qGreen(a) - qGreen(b)) + qAbs(qBlue(a) - qBlue(b));
}

// A box in page points -> the pixels it covers, kept inside the image.
QRect pixelArea(const QRectF &box, double sx, double sy, const Pixels &image)
{
    return QRectF(box.x() * sx, box.y() * sy, box.width() * sx, box.height() * sy)
        .toAlignedRect()
        .intersected(image.rect());
}

// Background color, ink color and ink weight inside `area`.
RegionStats analyzeRegion(const Pixels &image, const QRect &area)
{
    RegionStats stats;

    if (area.isEmpty())
        return stats;

    // At most ~20,000 pixels, evenly spread.
    const int step = qMax(1, int(std::sqrt(double(area.width()) * area.height() / 20000.0)));

    // 1) Background: the most common color (shades bucketed, so anti-aliasing doesn't split it).
    QHash<int, Sum> buckets;
    QVector<QRgb> samples;

    for (int y = area.top(); y <= area.bottom(); y += step) {
        for (int x = area.left(); x <= area.right(); x += step) {
            const QRgb color = image.at(x, y);
            samples.append(color);

            const int key = ((qRed(color) >> 3) << 10) | ((qGreen(color) >> 3) << 5)
                          | (qBlue(color) >> 3);
            Sum &sum = buckets[key];
            ++sum.count;
            sum.r += qRed(color);
            sum.g += qGreen(color);
            sum.b += qBlue(color);
        }
    }

    if (samples.isEmpty())
        return stats;

    const Sum *best = nullptr;
    for (auto it = buckets.constBegin(); it != buckets.constEnd(); ++it)
        if (!best || it.value().count > best->count)
            best = &it.value();

    if (!best || best->count <= 0)
        return stats;

    stats.background = QColor(int(best->r / best->count),
                              int(best->g / best->count),
                              int(best->b / best->count));
    stats.valid = true;

    const QRgb background = stats.background.rgb();

    // 2) Ink color: the pixels farthest from the background are the solid middle of the letters.
    //    Average the farthest fifth of that range, which ignores the blended pixels at the letter edges.
    int farthest = 0;
    for (const QRgb color : samples)
        farthest = qMax(farthest, distance(color, background));

    stats.farthest = farthest;

    if (farthest < 60) {
        // Nothing stands out (blank area): black or white, whichever reads better.
        stats.text = stats.background.lightness() > 128 ? QColor(Qt::black)
                                                        : QColor(Qt::white);
        return stats;
    }

    long long r = 0, g = 0, b = 0;
    int core = 0;
    int ink = 0;

    for (const QRgb color : samples) {
        const int d = distance(color, background);

        if (d >= farthest * 0.8) {
            r += qRed(color);
            g += qGreen(color);
            b += qBlue(color);
            ++core;
        }

        if (d >= farthest * 0.5)
            ++ink;
    }

    if (core > 0)
        stats.text = QColor(int(r / core), int(g / core), int(b / core));

    // 3) Ink weight: bold text covers more of its box than regular.
    stats.inkDensity = double(ink) / double(samples.size());

    return stats;
}

struct WordStats
{
    int farthest = 0;     // distance of the most different pixel from the background
    QColor text;          // ink color (invalid if the word has no clear ink)
    double stroke = 0.0;  // typical stroke thickness, as a fraction of the text size
};

// Measures one word against its line's background: ink color and stroke
// thickness (bold strokes are noticeably thicker at the same size).
WordStats measureWord(const Pixels &image,
                      const QRect &area,
                      const QColor &backgroundColor,
                      double sizePx)
{
    WordStats stats;

    if (area.isEmpty())
        return stats;

    const QRgb background = backgroundColor.rgb();

    int farthest = 0;
    for (int y = area.top(); y <= area.bottom(); ++y)
        for (int x = area.left(); x <= area.right(); ++x)
            farthest = qMax(farthest, distance(image.at(x, y), background));

    stats.farthest = farthest;
    if (farthest < 60)
        return stats;

    const int coreLevel = int(farthest * 0.8);
    const int inkLevel = int(farthest * 0.5);

    long long r = 0, g = 0, b = 0;
    int coreCount = 0;
    QVector<int> runs; // lengths of horizontal stretches of ink = stroke widths

    for (int y = area.top(); y <= area.bottom(); ++y) {
        int run = 0;

        for (int x = area.left(); x <= area.right(); ++x) {
            const QRgb color = image.at(x, y);
            const int d = distance(color, background);

            if (d >= coreLevel) {
                r += qRed(color);
                g += qGreen(color);
                b += qBlue(color);
                ++coreCount;
            }

            if (d >= inkLevel) {
                ++run;
            } else {
                if (run > 0)
                    runs.append(run);
                run = 0;
            }
        }

        if (run > 0)
            runs.append(run);
    }

    if (coreCount > 0)
        stats.text = QColor(int(r / coreCount), int(g / coreCount), int(b / coreCount));

    if (runs.size() >= 6 && sizePx > 0.0) {
        std::sort(runs.begin(), runs.end());
        stats.stroke = double(runs.at(int(runs.size()) / 2)) / sizePx;
    }

    return stats;
}

// Looks for a thin horizontal ink line just under the baseline, across most of the word.
// Modern underlines skip descenders, so gaps are allowed.
bool hasUnderlineBelow(const Pixels &image,
                       const TextWord &word,
                       double baselinePt,
                       double sizePt,
                       double sx,
                       double sy,
                       const QColor &backgroundColor,
                       int farthest)
{
    const int x0 = qMax(0, qRound(word.bounds.left() * sx));
    const int x1 = qMin(image.width() - 1, qRound(word.bounds.right() * sx));
    if (x1 - x0 < 3)
        return false;

    const QRgb background = backgroundColor.rgb();

    // An underline sits roughly 0.05 to 0.2 of the text size below the baseline.
    const int y0 = qRound((baselinePt + 0.04 * sizePt) * sy);
    const int y1 = qMax(y0, qRound((baselinePt + 0.22 * sizePt) * sy));
    const int threshold = qMax(30, int(farthest * 0.5));

    for (int y = y0; y <= y1; ++y) {
        if (y < 0 || y >= image.height())
            continue;

        int ink = 0;
        const int total = x1 - x0 + 1;

        for (int x = x0; x <= x1; ++x)
            if (distance(image.at(x, y), background) >= threshold)
                ++ink;

        if (ink >= 0.65 * total)
            return true;
    }

    return false;
}

} // namespace

void estimateLineStyles(ReconstructedPage &page, const QImage &pageImage)
{
    if (pageImage.isNull() || page.pageSize.width() <= 0.0 || page.pageSize.height() <= 0.0)
        return;

    const Pixels pixels(pageImage);
    if (pixels.width() <= 0 || pixels.height() <= 0)
        return;

    const double sx = pixels.width() / page.pageSize.width();
    const double sy = pixels.height() / page.pageSize.height();

    struct OcrWord
    {
        int block = 0;
        int line = 0;
        int word = 0;
        double stroke = 0.0;
    };

    QVector<OcrWord> measured; // OCR words whose boldness has to be guessed

    // Pass 1: each word gets its own ink color and underline, since a line can mix styles (a bold link, plain words, another link).
    for (int b = 0; b < int(page.blocks.size()); ++b) {
        for (int l = 0; l < int(page.blocks.at(b).lines.size()); ++l) {
            TextLine &line = page.blocks[b].lines[l];
            if (line.words.isEmpty())
                continue;

            const RegionStats lineStats =
                analyzeRegion(pixels, pixelArea(line.bounds, sx, sy, pixels));
            if (!lineStats.valid)
                continue;

            const bool hasInk = lineStats.farthest >= 60;

            double sizePt = 0.0;
            int sized = 0;
            for (const TextWord &word : line.words) {
                if (word.fontSize > 0.0f) {
                    sizePt += word.fontSize;
                    ++sized;
                }
            }
            sizePt = sized > 0 ? sizePt / sized : line.bounds.height() * 0.7;

            for (int w = 0; w < int(line.words.size()); ++w) {
                TextWord &word = line.words[w];

                WordStats stats;
                if (hasInk)
                    stats = measureWord(pixels,
                                        pixelArea(word.bounds, sx, sy, pixels),
                                        lineStats.background, sizePt * sy);

                word.color = stats.text.isValid() ? stats.text : lineStats.text;

                word.underlined = hasInk && stats.farthest >= 60
                    && hasUnderlineBelow(pixels, word, baselineYAt(line, word),
                                         sizePt, sx, sy, lineStats.background,
                                         stats.farthest);

                // Text-layer words already know whether their font is bold; OCR words (empty fontName) are judged by stroke thickness.
                if (word.fontName.isEmpty() && stats.stroke > 0.0 && word.text.size() >= 2) {
                    OcrWord entry;
                    entry.block = b;
                    entry.line = l;
                    entry.word = w;
                    entry.stroke = stats.stroke;
                    measured.append(entry);
                }
            }
        }
    }

    // Pass 2: an OCR word is bold if its strokes are clearly thicker than the
    // page's median. With only a few words the median isn't reliable, so don't guess.
    if (measured.size() < 8)
        return;

    QVector<double> strokes;
    for (const OcrWord &entry : measured)
        strokes.append(entry.stroke);

    std::sort(strokes.begin(), strokes.end());
    const double median = strokes.at(int(strokes.size()) / 2);
    if (median <= 0.0)
        return;

    for (const OcrWord &entry : measured)
        page.blocks[entry.block].lines[entry.line].words[entry.word].bold =
            entry.stroke > 1.35 * median;
}

void estimateAppearance(ReconstructedPage &page, const QImage &pageImage)
{
    if (pageImage.isNull() || page.pageSize.width() <= 0.0 || page.pageSize.height() <= 0.0)
        return;

    const Pixels pixels(pageImage);
    if (pixels.width() <= 0 || pixels.height() <= 0)
        return;

    const double sx = pixels.width() / page.pageSize.width();
    const double sy = pixels.height() / page.pageSize.height();

    for (TextParagraph &paragraph : page.paragraphs) {
        const RegionStats stats =
            analyzeRegion(pixels, pixelArea(paragraph.bounds, sx, sy, pixels));

        if (!stats.valid)
            continue;

        paragraph.backgroundColor = stats.background;
        paragraph.textColor = stats.text;
    }
}

}