#pragma once

#include <QColor>
#include <QString>
#include <QLineF>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QSizeF>
#include <QVector>

namespace Reconstruction {

// Every `bounds` rectangle is in PDF page points (1/72 inch), origin top-left, y down, independent of the resolution the page was rasterized at.

// Unknown = could not tell; the app should fall back to its default face.
enum class FontFamily { Unknown = 0, Sans, Serif, Mono };

struct TextWord
{
    QString text;
    QRectF bounds;
    float confidence = 0.0f; // 0..100 (Tesseract); 100 for text-layer words
    float fontSize = 0.0f;   // points: exact for text layer, estimated for OCR, 0 = unknown
    QString fontName;        // text layer only; empty marks an OCR word
    bool bold = false;       // text layer: PDF font; OCR: Tesseract flag or stroke width
    bool italic = false;
    FontFamily family = FontFamily::Unknown;
    QColor color;            // invalid until estimateLineStyles() runs
    bool underlined = false; // detected from the pixels under the text
};

struct TextLine
{
    QRectF bounds;
    QLineF baseline;         // null if unknown
    QVector<TextWord> words;
};

struct TextBlock
{
    QRectF bounds;
    QVector<TextLine> lines;
};

// Text in one uniform style. A paragraph is a list of runs, so an edit keeps the bold heading, the underlined link, and so on.
struct TextRun
{
    QString text;
    QString fontName;         // original PDF font name, if known
    float fontSize = 0.0f;    // points
    bool bold = false;
    bool italic = false;
    FontFamily family = FontFamily::Unknown;
    bool underlined = false;
    QColor color = QColor(Qt::black);
};

// Points at page.blocks[block].lines[line].
struct LineRef
{
    int block = 0;
    int line = 0;
};

// The unit a user edits. Refers to lines in page.blocks instead of copying them.
// Built by groupParagraphs(); colors come from estimateAppearance().
struct TextParagraph
{
    QRectF bounds;            // union of its lines' boxes
    QVector<LineRef> lines;   // reading order
    QString originalText;
    QString text;             // current text; equals originalText until edited
    QVector<TextRun> runs;    // `text` split into styled pieces (same characters)
    double markerGap = 0.0;   // list item: points from marker to text (0 = no marker)

    // Points from the paragraph's left edge. A list item hangs: firstLineIndent
    // is 0 and leftIndent equals markerGap.
    double firstLineIndent = 0.0;
    double leftIndent = 0.0;  // where lines after the first one start
    float fontSize = 0.0f;    // average word size, points (0 = unknown)
    bool edited = false;

    QString fontName;         // dominant font; text-layer pages only
    bool bold = false;
    bool italic = false;
    FontFamily family = FontFamily::Unknown; // most common among its words
    double lineSpacing = 0.0; // baseline to baseline, points
    Qt::AlignmentFlag alignment = Qt::AlignLeft; // AlignLeft / AlignHCenter / AlignRight
    QColor textColor = QColor(Qt::black);
    QColor backgroundColor = QColor(Qt::white);
};

struct ReconstructedPage
{
    int pageIndex = 0;
    QSizeF pageSize;  // points
    QSize imageSize;  // pixels of the OCR'd image

    QVector<TextBlock> blocks;
    QVector<TextParagraph> paragraphs;
};

// An out-of-range reference (stale after the page was rebuilt) gives an empty line.
inline const TextLine &lineAt(const ReconstructedPage &page, const LineRef &ref)
{
    static const TextLine empty{};

    if (ref.block < 0 || ref.block >= int(page.blocks.size()))
        return empty;

    const TextBlock &block = page.blocks.at(ref.block);
    if (ref.line < 0 || ref.line >= int(block.lines.size()))
        return empty;

    return block.lines.at(ref.line);
}

// Baseline y (page points) at the word's left edge. Without a baseline, falls back to a point just above the bottom of the word's box.
inline double baselineYAt(const TextLine &line, const TextWord &word)
{
    const QLineF &b = line.baseline;
    const double dx = b.x2() - b.x1();

    if (b.isNull() || qAbs(dx) < 1e-6)
        return word.bounds.bottom() - word.bounds.height() * 0.2;

    const double t = (word.bounds.left() - b.x1()) / dx;
    return b.y1() + t * (b.y2() - b.y1());
}

// Baseline y of a paragraph's first line, where its text starts.
inline double paragraphFirstBaselineY(const ReconstructedPage &page,
                                      const TextParagraph &paragraph)
{
    if (paragraph.lines.isEmpty())
        return paragraph.bounds.bottom();

    const TextLine &line = lineAt(page, paragraph.lines.first());
    if (line.words.isEmpty())
        return line.bounds.bottom();

    return baselineYAt(line, line.words.first());
}

// The word whose box contains pagePoint, or nullptr. The pointer is valid until the page's lists change size.
inline TextWord *findWordAt(ReconstructedPage &page, const QPointF &pagePoint)
{
    for (TextBlock &block : page.blocks)
        for (TextLine &line : block.lines)
            for (TextWord &word : line.words)
                if (word.bounds.contains(pagePoint))
                    return &word;

    return nullptr;
}

// How far a single line may grow past its own box while edited: the larger of a fixed amount and a share of its width.
inline constexpr double kEditSlackMin = 36.0;     // points
inline constexpr double kEditSlackFactor = 0.5;   // of the line's own width

// The rectangle (page points) where a paragraph's text is laid out while edited.
// Multi-line paragraphs keep their column width. A single line may grow toward
// the page margin but stops before other paragraphs on the same row.
inline QRectF editRegion(const ReconstructedPage &page, const TextParagraph &paragraph)
{
    QRectF region = paragraph.bounds;
    const double pageWidth = page.pageSize.width();

    if (paragraph.lines.size() > 1 || pageWidth <= 0.0) {
        // A little extra width: a font that measures slightly wider would wrap
        // the longest line's last word and push everything below it down.
        if (pageWidth > 0.0) {
            const double room = qMax(3.0, region.width() * 0.03);
            switch (paragraph.alignment) {
            case Qt::AlignRight:
                region.setLeft(qMax(0.0, region.left() - room));
                break;
            case Qt::AlignHCenter:
                region.setLeft(qMax(0.0, region.left() - room / 2.0));
                region.setRight(qMin(pageWidth, region.right() + room / 2.0));
                break;
            default:
                region.setRight(qMin(pageWidth, region.right() + room));
                break;
            }
        }
        return region;
    }

    const double margin = qMax(18.0, qMin(72.0, qMin(region.left(), pageWidth - region.right())));

    double leftLimit = margin;
    double rightLimit = pageWidth - margin;
    constexpr double kGap = 2.0;

    for (const TextParagraph &other : page.paragraphs) {
        if (&other == &paragraph)
            continue;

        // Only paragraphs that really share this row count.
        const double overlap = qMin(other.bounds.bottom(), region.bottom())
                             - qMax(other.bounds.top(), region.top());
        if (overlap < 0.4 * region.height())
            continue;

        if (other.bounds.left() >= region.right() - 0.5)
            rightLimit = qMin(rightLimit, other.bounds.left() - kGap);
        else if (other.bounds.right() <= region.left() + 0.5)
            leftLimit = qMax(leftLimit, other.bounds.right() + kGap);
    }

    // The limits only restrict growth; they never shrink the paragraph.
    leftLimit = qMin(leftLimit, region.left());
    rightLimit = qMax(rightLimit, region.right());

    const double slack = qMax(kEditSlackMin, region.width() * kEditSlackFactor);
    leftLimit = qMax(leftLimit, region.left() - slack);
    rightLimit = qMin(rightLimit, region.right() + slack);

    switch (paragraph.alignment) {
    case Qt::AlignRight:
        region.setLeft(leftLimit);
        break;
    case Qt::AlignHCenter: {
        const double center = region.center().x();
        const double half = qMin(center - leftLimit, rightLimit - center);
        if (half * 2.0 > region.width()) {
            region.setLeft(center - half);
            region.setRight(center + half);
        }
        break;
    }
    default:
        region.setRight(rightLimit);
        break;
    }

    return region;
}

// Free vertical space under a paragraph, up to the next paragraph below it that
// overlaps sideways, or the page bottom.
inline double roomBelow(const ReconstructedPage &page, const TextParagraph &paragraph)
{
    double limit = page.pageSize.height();

    for (const TextParagraph &other : page.paragraphs) {
        if (&other == &paragraph)
            continue;

        const bool isBelow = other.bounds.center().y() > paragraph.bounds.center().y();
        const bool overlapsSideways = other.bounds.left() < paragraph.bounds.right()
                                   && other.bounds.right() > paragraph.bounds.left();

        if (isBelow && overlapsSideways)
            limit = qMin(limit, other.bounds.top());
    }

    return qMax(0.0, limit - paragraph.bounds.bottom());
}

// Index of the paragraph under pagePoint, or -1. Boxes can overlap; the smallest
// wins, as the more specific thing under the cursor.
inline int paragraphIndexAt(const ReconstructedPage &page, const QPointF &pagePoint)
{
    int best = -1;
    double bestArea = 0.0;

    for (int i = 0; i < int(page.paragraphs.size()); ++i) {
        const QRectF &box = page.paragraphs.at(i).bounds;
        if (!box.contains(pagePoint))
            continue;

        const double area = box.width() * box.height();
        if (best < 0 || area < bestArea) {
            best = i;
            bestArea = area;
        }
    }

    return best;
}

inline QString joinRuns(const QVector<TextRun> &runs)
{
    QString text;
    for (const TextRun &run : runs)
        text += run.text;
    return text;
}

// Stores an edited paragraph's new runs. Refer to a paragraph by INDEX, not
// pointer: pointers dangle when the page is replaced or regrouped. Returns false
// if the index no longer exists.
inline bool applyParagraphEdit(ReconstructedPage &page,
                               int index,
                               const QVector<TextRun> &runs)
{
    if (index < 0 || index >= int(page.paragraphs.size()))
        return false;

    TextParagraph &paragraph = page.paragraphs[index];
    const QString newText = joinRuns(runs);

    paragraph.runs = runs;
    paragraph.text = newText;
    paragraph.edited = (newText != paragraph.originalText);
    return true;
}

inline TextParagraph *findParagraphAt(ReconstructedPage &page, const QPointF &pagePoint)
{
    const int index = paragraphIndexAt(page, pagePoint);
    return index < 0 ? nullptr : &page.paragraphs[index];
}

}