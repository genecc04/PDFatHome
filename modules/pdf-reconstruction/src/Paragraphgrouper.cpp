#include "reconstruction/ParagraphGrouper.h"

#include <QHash>
#include <QtGlobal>

namespace Reconstruction {

namespace {

const ushort kBulletCode = 0x2022;

// Average word size; without reported sizes, estimated from the line height.
double lineFontSize(const TextLine &line)
{
    double sum = 0.0;
    int count = 0;

    for (const TextWord &word : line.words) {
        if (word.fontSize > 0.0f) {
            sum += word.fontSize;
            ++count;
        }
    }

    if (count > 0)
        return sum / count;

    return line.bounds.height() * 0.7;
}

double lineBaselineY(const TextLine &line)
{
    if (line.words.isEmpty())
        return line.bounds.bottom();

    return baselineYAt(line, line.words.first());
}

// True if nearly every word has the property (a bold heading, not one bold word).
bool mostWords(const TextLine &line, bool TextWord::*property)
{
    if (line.words.isEmpty())
        return false;

    int count = 0;
    for (const TextWord &word : line.words)
        if (word.*property)
            ++count;

    return count * 10 >= int(line.words.size()) * 8;
}

QColor lineColor(const TextLine &line)
{
    for (const TextWord &word : line.words)
        if (word.color.isValid())
            return word.color;

    return QColor();
}

int colorDistance(const QColor &a, const QColor &b)
{
    return qAbs(a.red() - b.red()) + qAbs(a.green() - b.green()) + qAbs(a.blue() - b.blue());
}

// list markers (bullet, "-", "1.")

bool isBulletChar(const QString &text)
{
    if (text.size() != 1)
        return false;

    switch (text.at(0).unicode()) {
    case 0x2022: case 0x00B7: case 0x25CF: case 0x25CB:
    case 0x25E6: case 0x25AA: case 0x25A0: case 0x2013:
    case 0x2014: case '*': case '-':
        return true;
    default:
        return false;
    }
}

// OCR often reads a round bullet as one of these.
bool isMisreadBullet(const QString &text)
{
    if (text == QLatin1String("e") || text == QLatin1String("o"))
        return true;

    if (text.size() == 1) {
        const ushort c = text.at(0).unicode();
        return c == 0x00A9 || c == 0x00B0 || c == 0x00BB;
    }

    return false;
}

bool isNumberMarker(const QString &text)
{
    if (text.size() < 2 || text.size() > 4)
        return false;

    const QChar last = text.at(text.size() - 1);
    if (last != QLatin1Char('.') && last != QLatin1Char(')'))
        return false;

    for (int i = 0; i < text.size() - 1; ++i)
        if (!text.at(i).isDigit())
            return false;

    return true;
}

// A list marker followed by a visible gap. It always starts a new paragraph.
bool startsWithMarker(const TextLine &line)
{
    if (line.words.size() < 2)
        return false;

    const TextWord &first = line.words.at(0);
    const TextWord &second = line.words.at(1);
    const double size = lineFontSize(line);
    const double gap = second.bounds.left() - first.bounds.right();

    if (isBulletChar(first.text) || isNumberMarker(first.text))
        return gap > 0.3 * size;

    if (isMisreadBullet(first.text))
        return gap > 0.5 * size;

    return false;
}

// paragraph contents 

TextRun styleOf(const TextWord &word, double fallbackSize)
{
    TextRun run;
    run.fontName = word.fontName;
    run.fontSize = word.fontSize > 0.0f ? word.fontSize : float(fallbackSize);
    run.bold = word.bold;
    run.italic = word.italic;
    run.family = word.family;
    run.underlined = word.underlined;
    run.color = word.color.isValid() ? word.color : QColor(Qt::black);
    return run;
}

bool sameStyle(const TextRun &a, const TextRun &b)
{
    return a.fontName == b.fontName
        && qAbs(a.fontSize - b.fontSize) < 0.25f
        && a.bold == b.bold
        && a.italic == b.italic
        && a.family == b.family
        && a.underlined == b.underlined
        // Colors are averaged from anti-aliased pixels, so compare with a tolerance or every word becomes its own run.
        && colorDistance(a.color, b.color) <= 90;
}

// Running text (every line but the last nearly fills the width) may be re-wrapped when edited; a stack of short lines (heading, address) keeps its breaks.
bool isFlowingText(const ReconstructedPage &page, const TextParagraph &paragraph)
{
    const int count = int(paragraph.lines.size());
    if (count < 2)
        return true; // no line breaks to keep

    double left = 1e9;
    double right = -1e9;
    for (const LineRef &ref : paragraph.lines) {
        const QRectF &box = lineAt(page, ref).bounds;
        left = qMin(left, box.left());
        right = qMax(right, box.right());
    }

    const double width = right - left;
    if (width <= 0.0)
        return true;

    for (int i = 0; i < count - 1; ++i) {
        if (lineAt(page, paragraph.lines.at(i)).bounds.width() < 0.75 * width)
            return false;
    }
    return true;
}

// The font name if every word on the line has the same font, else empty.
QString uniformFontName(const TextLine &line)
{
    QString name;
    for (const TextWord &word : line.words) {
        if (word.fontName.isEmpty())
            return QString();
        if (name.isEmpty())
            name = word.fontName;
        else if (word.fontName != name)
            return QString();
    }
    return name;
}

// Fills paragraph.runs / text / originalText / markerGap from its lines.
void buildRuns(const ReconstructedPage &page, TextParagraph &paragraph, bool keepLineBreaks)
{
    QVector<TextRun> runs;

    auto append = [&runs](const TextRun &style, const QString &text) {
        if (!runs.isEmpty() && sameStyle(runs.last(), style)) {
            runs.last().text += text;
        } else {
            TextRun run = style;
            run.text = text;
            runs.append(run);
        }
    };

    const int lineCount = int(paragraph.lines.size());
    const bool flowing = !keepLineBreaks && isFlowingText(page, paragraph);

    for (int li = 0; li < lineCount; ++li) {
        const TextLine &line = lineAt(page, paragraph.lines.at(li));
        const double fallbackSize = lineFontSize(line);
        const bool marker = (li == 0) && startsWithMarker(line);
        const int wordCount = int(line.words.size());

        if (marker) {
            paragraph.markerGap = line.words.at(1).bounds.left()
                                - line.words.at(0).bounds.left();
        }

        for (int wi = 0; wi < wordCount; ++wi) {
            const TextWord &word = line.words.at(wi);

            QString text = word.text;
            if (marker && wi == 0 && isMisreadBullet(text))
                text = QString(QChar(kBulletCode));

            const bool lastWord = (li == lineCount - 1) && (wi == wordCount - 1);

            // Separator: a space between words and lines (the paragraph wraps again);
            // a newline where the original broke a line, unless the text
            // flows; a tab after a list marker, which keeps the item's text in place.
            QString separator = lastWord ? QString() : QStringLiteral(" ");

            if (!flowing && wi == wordCount - 1 && li < lineCount - 1)
                separator = QStringLiteral("\n");
            if (marker && wi == 0)
                separator = QStringLiteral("\t");

            append(styleOf(word, fallbackSize), text + separator);
        }
    }

    QString text;
    for (const TextRun &run : runs)
        text += run.text;

    paragraph.runs = runs;
    paragraph.text = text;
    paragraph.originalText = text;
}

// Works out the paragraph's dominant font, line spacing, indents and alignment.
void describeStyle(const ReconstructedPage &page, TextParagraph &paragraph)
{
    QHash<QString, int> counts;
    int bestCount = 0;

    for (const LineRef &ref : paragraph.lines) {
        for (const TextWord &word : lineAt(page, ref).words) {
            if (word.fontName.isEmpty())
                continue;

            const int count = ++counts[word.fontName];
            if (count > bestCount) {
                bestCount = count;
                paragraph.fontName = word.fontName;
                paragraph.bold = word.bold;
                paragraph.italic = word.italic;
            }
        }
    }

    // OCR words have no font name, so family and slant are a vote over all words.
    int familyVotes[4] = {0, 0, 0, 0};
    int italicWords = 0;
    int allWords = 0;

    for (const LineRef &ref : paragraph.lines) {
        for (const TextWord &word : lineAt(page, ref).words) {
            ++allWords;
            if (word.italic)
                ++italicWords;
            ++familyVotes[int(word.family)];
        }
    }

    int bestFamily = int(FontFamily::Unknown);
    int bestVotes = 0;
    for (int f = 1; f < 4; ++f) {
        if (familyVotes[f] > bestVotes) {
            bestVotes = familyVotes[f];
            bestFamily = f;
        }
    }

    paragraph.family = FontFamily(bestFamily);
    if (allWords > 0 && italicWords * 2 > allWords)
        paragraph.italic = true;

    double distanceSum = 0.0;
    for (int i = 1; i < int(paragraph.lines.size()); ++i) {
        distanceSum += lineBaselineY(lineAt(page, paragraph.lines.at(i)))
                     - lineBaselineY(lineAt(page, paragraph.lines.at(i - 1)));
    }

    if (paragraph.lines.size() > 1) {
        paragraph.lineSpacing = distanceSum / double(paragraph.lines.size() - 1);
    } else {
        paragraph.lineSpacing = paragraph.fontSize > 0.0f
            ? double(paragraph.fontSize) * 1.2
            : paragraph.bounds.height() * 0.85;
    }

    const double left = paragraph.bounds.left();
    paragraph.firstLineIndent =
        qMax(0.0, lineAt(page, paragraph.lines.first()).bounds.left() - left);

    if (paragraph.lines.size() > 1) {
        double indentSum = 0.0;
        for (int i = 1; i < int(paragraph.lines.size()); ++i)
            indentSum += lineAt(page, paragraph.lines.at(i)).bounds.left() - left;

        paragraph.leftIndent = qMax(0.0, indentSum / double(paragraph.lines.size() - 1));
    } else {
        // One line can't show where wrapped lines would go, but a list item's always line up with the text after the marker.
        paragraph.leftIndent = paragraph.markerGap > 0.0 ? paragraph.markerGap
                                                         : paragraph.firstLineIndent;
    }

    // Alignment: which edge (left, right or middle) do all the lines share?
    const double tolerance = qMax(2.0, 0.6 * double(paragraph.fontSize));
    const double pageWidth = page.pageSize.width();

    if (paragraph.lines.size() > 1) {
        double minLeft = 1e9, maxLeft = -1e9;
        double minRight = 1e9, maxRight = -1e9;
        double minMid = 1e9, maxMid = -1e9;

        for (const LineRef &ref : paragraph.lines) {
            const QRectF &box = lineAt(page, ref).bounds;
            minLeft = qMin(minLeft, box.left());
            maxLeft = qMax(maxLeft, box.left());
            minRight = qMin(minRight, box.right());
            maxRight = qMax(maxRight, box.right());
            minMid = qMin(minMid, box.center().x());
            maxMid = qMax(maxMid, box.center().x());
        }

        const double leftSpread = maxLeft - minLeft;

        if (leftSpread <= tolerance)
            paragraph.alignment = Qt::AlignLeft;
        else if (leftSpread > 3.0 * tolerance && (maxRight - minRight) <= tolerance)
            paragraph.alignment = Qt::AlignRight;
        else if (leftSpread > 3.0 * tolerance && (maxMid - minMid) <= tolerance)
            paragraph.alignment = Qt::AlignHCenter;
        else
            paragraph.alignment = Qt::AlignLeft;
    } else if (pageWidth > 0.0) {
        // One line can't show its alignment, but a line in the middle of the page, away from the margins, is almost always centered.
        const bool inMiddle = qAbs(paragraph.bounds.center().x() - pageWidth / 2.0) < 4.0;
        paragraph.alignment = (inMiddle && paragraph.bounds.left() > 0.15 * pageWidth)
            ? Qt::AlignHCenter
            : Qt::AlignLeft;
    }
}

// Does `next` look like the continuation of `previous`?
bool continuesParagraph(const TextLine &previous,
                        const TextLine &next,
                        const ParagraphOptions &options)
{
    const double previousSize = lineFontSize(previous);
    const double nextSize = lineFontSize(next);
    const double bigger = qMax(previousSize, nextSize);

    if (bigger <= 0.0)
        return false;

    // Heading vs body text.
    if (qAbs(previousSize - nextSize) / bigger > options.maxSizeChange)
        return false;

    // A different look: bold vs regular, link vs plain text.
    if (mostWords(previous, &TextWord::bold) != mostWords(next, &TextWord::bold))
        return false;

    if (mostWords(previous, &TextWord::underlined) != mostWords(next, &TextWord::underlined))
        return false;

    // Both lines in a single font, but different fonts (heading above sub-heading).
    const QString previousFont = uniformFontName(previous);
    const QString nextFont = uniformFontName(next);
    if (!previousFont.isEmpty() && !nextFont.isEmpty() && previousFont != nextFont)
        return false;

    const QColor previousColor = lineColor(previous);
    const QColor nextColor = lineColor(next);
    if (previousColor.isValid() && nextColor.isValid()
        && colorDistance(previousColor, nextColor) > options.maxColorChange)
        return false;

    if (startsWithMarker(next))
        return false;

    // The next line must sit below, and not too far below (zero or less means beside it, e.g. another column).
    const double distance = lineBaselineY(next) - lineBaselineY(previous);
    if (distance <= 0.0 || distance > options.maxLineSpacing * bigger)
        return false;

    // A big sideways jump means another column or item. After a list marker,
    // continuation lines line up with the item's text, not the marker.
    if (startsWithMarker(previous)) {
        const double textLeft = previous.words.at(1).bounds.left();
        if (qAbs(next.bounds.left() - textLeft) > 1.5 * bigger)
            return false;
    } else if (qAbs(next.bounds.left() - previous.bounds.left()) > options.maxLeftShift * bigger) {
        return false;
    }

    return true;
}

} // namespace

void splitLinesAtGaps(ReconstructedPage &page, double maxGapInFontSizes)
{
    if (maxGapInFontSizes <= 0.0)
        return;

    for (TextBlock &block : page.blocks) {
        QVector<TextLine> lines;
        lines.reserve(block.lines.size());

        for (const TextLine &line : block.lines) {
            const double limit = maxGapInFontSizes * lineFontSize(line);

            if (line.words.size() < 2 || limit <= 0.0) {
                lines.append(line);
                continue;
            }

            TextLine piece;
            piece.baseline = line.baseline; // same line equation for every piece

            auto finishPiece = [&]() {
                if (piece.words.isEmpty())
                    return;

                QRectF wordsBox = piece.words.first().bounds;
                for (const TextWord &word : piece.words)
                    wordsBox = wordsBox.united(word.bounds);

                // Keep the line's top and height; take only the horizontal extent of this piece's words.
                piece.bounds = line.bounds.isEmpty()
                    ? wordsBox
                    : QRectF(wordsBox.left(), line.bounds.top(),
                             wordsBox.width(), line.bounds.height());

                lines.append(piece);
                piece = TextLine();
                piece.baseline = line.baseline;
            };

            for (const TextWord &word : line.words) {
                if (!piece.words.isEmpty()) {
                    const TextWord &previous = piece.words.last();
                    const double gap = word.bounds.left() - previous.bounds.right();

                    // The space after a list marker can be wide but belongs to the item.
                    const bool afterMarker = piece.words.size() == 1
                        && (isBulletChar(previous.text) || isNumberMarker(previous.text)
                            || isMisreadBullet(previous.text));

                    if (gap > limit && !afterMarker)
                        finishPiece();
                }

                piece.words.append(word);
            }

            finishPiece();
        }

        block.lines = lines;
    }
}

void groupParagraphs(ReconstructedPage &page, const ParagraphOptions &options)
{
    page.paragraphs.clear();

    splitLinesAtGaps(page, options.maxWordGap);

    TextParagraph current;
    bool open = false;
    bool haveBounds = false;
    double sizeSum = 0.0;
    int sizeCount = 0;
    int previousBlock = -1;
    const TextLine *previousLine = nullptr;

    auto finish = [&]() {
        if (open) {
            current.fontSize = sizeCount > 0 ? float(sizeSum / sizeCount) : 0.0f;
            buildRuns(page, current, options.keepLineBreaks);
            describeStyle(page, current);
            page.paragraphs.append(current);
        }

        current = TextParagraph();
        open = false;
        haveBounds = false;
        sizeSum = 0.0;
        sizeCount = 0;
        previousLine = nullptr;
    };

    for (int b = 0; b < int(page.blocks.size()); ++b) {
        const TextBlock &block = page.blocks.at(b);

        for (int l = 0; l < int(block.lines.size()); ++l) {
            const TextLine &line = block.lines.at(l);

            if (line.words.isEmpty())
                continue;

            const bool newBlock = (b != previousBlock);

            if (open) {
                const bool split =
                    (newBlock && options.respectBlocks) ||
                    !previousLine ||
                    !continuesParagraph(*previousLine, line, options);

                if (split)
                    finish();
            }

            LineRef ref;
            ref.block = b;
            ref.line = l;
            current.lines.append(ref);

            current.bounds = haveBounds ? current.bounds.united(line.bounds)
                                        : line.bounds;
            haveBounds = true;

            for (const TextWord &word : line.words) {
                if (word.fontSize > 0.0f) {
                    sizeSum += word.fontSize;
                    ++sizeCount;
                }
            }

            open = true;
            previousBlock = b;
            previousLine = &line;
        }
    }

    finish();
}

}