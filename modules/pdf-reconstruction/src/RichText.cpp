#include "reconstruction/RichText.h"
#include "reconstruction/FontClassifier.h"

#include <QAbstractTextDocumentLayout>
#include <QFontDatabase>
#include <QHash>
#include <QStringList>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextFragment>
#include <QTextLayout>
#include <QTextLine>
#include <QTextOption>
#include <QtGlobal>

namespace Reconstruction {

namespace {

// Lower-case letters and digits only, so "Segoe UI", "SegoeUI" and "segoe-ui" compare equal.
QString normalizedName(const QString &name)
{
    QString result;
    for (const QChar ch : name) {
        if (ch.isLetterOrNumber())
            result += ch.toLower();
    }
    return result;
}

// Families the standard PDF fonts already cover (metric-compatible).
bool isStandardFamily(const QString &family)
{
    const QString key = normalizedName(family);
    return key == QLatin1String("arial") || key == QLatin1String("timesnewroman")
        || key == QLatin1String("couriernew");
}

}

QString installedFamilyFor(const QString &pdfFontName)
{
    if (pdfFontName.isEmpty())
        return {};

    // Only the GUI thread calls this, so the cache needs no lock.
    static QHash<QString, QString> cache;
    const auto cached = cache.constFind(pdfFontName);
    if (cached != cache.constEnd())
        return cached.value();

    // PDF names run family and style together ("Inter-Regular", "ArialMT", "Calibri,Bold").
    // The installed family whose name the PDF name starts with is the match; the longest wins ("Calibri Light" over "Calibri").
    const QString wanted = normalizedName(pdfFontName);
    QString best;
    int bestLength = 0;
    const QStringList families = QFontDatabase::families();
    for (const QString &family : families) {
        const QString key = normalizedName(family);
        if (key.size() < 3 || key.size() <= bestLength || !wanted.startsWith(key))
            continue;
        best = family;
        bestLength = int(key.size());
    }

    cache.insert(pdfFontName, best);
    return best;
}

QFont qtFont(const QString &pdfFontName, FontFamily family, bool bold, bool italic,
             double pixelSize)
{
    const FontStyle guess = classifyFont(pdfFontName);
    if (family == FontFamily::Unknown)
        family = guess.family;

    QString name = QStringLiteral("Arial");
    if (family == FontFamily::Mono)
        name = QStringLiteral("Courier New");
    else if (family == FontFamily::Serif)
        name = QStringLiteral("Times New Roman");

    const QString installed = installedFamilyFor(pdfFontName);
    if (!installed.isEmpty())
        name = installed;

    QFont font(name);
    font.setPixelSize(qMax(6, qRound(pixelSize)));
    // `bold` and `italic` already include what the font name says (see mergeFontEvidence). 
    // Don't OR the name guess in again: that made it impossible to turn bold or italic off on text from a bold font.
    font.setBold(bold);
    font.setItalic(italic);
    return font;
}

QTextCharFormat charFormat(const TextRun &run, double pixelsPerPoint)
{
    QTextCharFormat format;
    format.setFont(qtFont(run.fontName, run.family, run.bold, run.italic,
                          run.fontSize * pixelsPerPoint));
    format.setForeground(run.color);
    format.setFontUnderline(run.underlined);
    format.setProperty(kFontNameProperty, run.fontName);
    format.setProperty(kFontSizeProperty, double(run.fontSize));
    format.setProperty(kFontFamilyProperty, int(run.family));
    return format;
}

QVector<TextRun> runsOf(const TextParagraph &paragraph)
{
    if (!paragraph.runs.isEmpty())
        return paragraph.runs;

    TextRun run;
    run.text = paragraph.text;
    run.fontName = paragraph.fontName;
    run.fontSize = paragraph.fontSize > 0.0f ? paragraph.fontSize
                                             : float(paragraph.bounds.height() * 0.5);
    run.bold = paragraph.bold;
    run.italic = paragraph.italic;
    run.family = paragraph.family;
    run.color = paragraph.textColor;
    return {run};
}

double lineSpacingFor(const TextParagraph &paragraph, double biggestSizePoints)
{
    const double sizePt = paragraph.fontSize > 0.0f ? double(paragraph.fontSize)
                                                    : paragraph.bounds.height() * 0.5;
    double spacing = paragraph.lineSpacing > 0.0 ? paragraph.lineSpacing : sizePt * 1.2;

    // Text bigger than the original needs more room (5% slack for rounding).
    if (sizePt > 0.0 && biggestSizePoints > sizePt * 1.05)
        spacing *= biggestSizePoints / sizePt;

    return spacing;
}

std::unique_ptr<QTextDocument> makeDocument(const TextParagraph &paragraph,
                                            double pixelsPerPoint, double widthPx)
{
    auto document = std::make_unique<QTextDocument>();
    document->setDocumentMargin(0);

    QTextOption option;
    option.setWrapMode(QTextOption::WordWrap);
    option.setAlignment(paragraph.alignment);
    if (paragraph.markerGap > 0.0)
        option.setTabStopDistance(paragraph.markerGap * pixelsPerPoint);
    document->setDefaultTextOption(option);
    document->setTextWidth(widthPx);

    const QVector<TextRun> runs = runsOf(paragraph);

    double biggestPt = 0.0;
    for (const TextRun &run : runs)
        biggestPt = qMax(biggestPt, double(run.fontSize));
    const double spacingPt = lineSpacingFor(paragraph, biggestPt);

    QTextBlockFormat blockFormat;
    blockFormat.setLineHeight(spacingPt * pixelsPerPoint, QTextBlockFormat::FixedHeight);
    blockFormat.setAlignment(paragraph.alignment);

    QTextCursor cursor(document.get());
    cursor.setBlockFormat(blockFormat);

    for (const TextRun &run : runs) {
        const QStringList parts = run.text.split(QLatin1Char('\n'));

        for (int i = 0; i < int(parts.size()); ++i) {
            if (i > 0)
                cursor.insertBlock(blockFormat);

            if (!parts.at(i).isEmpty())
                cursor.insertText(parts.at(i), charFormat(run, pixelsPerPoint));
        }
    }

    // Building the text counts as a modification; the caller wants "unchanged".
    document->setModified(false);
    return document;
}

double firstBaselineOffset(const QTextDocument *document)
{
    const QTextBlock block = document->firstBlock();

    double offset = document->documentLayout()->blockBoundingRect(block).top();
    if (block.layout() && block.layout()->lineCount() > 0) {
        const QTextLine line = block.layout()->lineAt(0);
        offset += line.y() + line.ascent();
    }

    return offset;
}

QVector<TextRun> runsFromDocument(const QTextDocument *document, double pixelsPerPoint)
{
    QVector<TextRun> runs;
    TextRun lastStyle;

    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid())
                continue;

            const QTextCharFormat format = fragment.charFormat();

            TextRun run;
            run.text = fragment.text();
            run.fontName = format.property(kFontNameProperty).toString();
            run.fontSize = float(format.property(kFontSizeProperty).toDouble());
            if (run.fontSize <= 0.0f && pixelsPerPoint > 0.0)
                run.fontSize = float(format.font().pixelSize() / pixelsPerPoint);
            run.bold = format.font().bold();
            run.italic = format.font().italic();
            run.family = FontFamily(format.property(kFontFamilyProperty).toInt());
            run.underlined = format.fontUnderline();
            run.color = format.foreground().color();

            lastStyle = run;
            runs.append(run);
        }

        // Between blocks: the line break the user typed.
        if (block.next().isValid()) {
            TextRun newline = lastStyle;
            newline.text = QStringLiteral("\n");
            runs.append(newline);
        }
    }

    return runs;
}

QString pdfBaseFont(FontFamily family, bool bold, bool italic)
{
    if (family == FontFamily::Mono) {
        if (bold && italic) return QStringLiteral("Courier-BoldOblique");
        if (bold)           return QStringLiteral("Courier-Bold");
        if (italic)         return QStringLiteral("Courier-Oblique");
        return QStringLiteral("Courier");
    }
    if (family == FontFamily::Serif) {
        if (bold && italic) return QStringLiteral("Times-BoldItalic");
        if (bold)           return QStringLiteral("Times-Bold");
        if (italic)         return QStringLiteral("Times-Italic");
        return QStringLiteral("Times-Roman");
    }
    if (bold && italic) return QStringLiteral("Helvetica-BoldOblique");
    if (bold)           return QStringLiteral("Helvetica-Bold");
    if (italic)         return QStringLiteral("Helvetica-Oblique");
    return QStringLiteral("Helvetica");
}

QVector<PlacedSpan> placeSpans(const TextParagraph &paragraph, const QRectF &region,
                               double firstBaselineY)
{
    // Lay the text out large, so rounding of small font sizes cannot change where lines wrap or how wide they are.
    constexpr double k = 4.0; // layout pixels per point

    const std::unique_ptr<QTextDocument> document =
        makeDocument(paragraph, k, region.width() * k);
    QAbstractTextDocumentLayout *layout = document->documentLayout();
    layout->documentSize();

    const double top = firstBaselineY - firstBaselineOffset(document.get()) / k;

    QVector<PlacedSpan> spans;

    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        const QTextLayout *textLayout = block.layout();
        if (!textLayout)
            continue;

        const QPointF blockPos = layout->blockBoundingRect(block).topLeft();
        const QString blockText = block.text();

        for (int li = 0; li < textLayout->lineCount(); ++li) {
            const QTextLine line = textLayout->lineAt(li);
            const int lineStart = line.textStart();
            const int lineEnd = lineStart + line.textLength();
            const double baselineY = top + (blockPos.y() + line.y() + line.ascent()) / k;

            // Justified text is spread over the line by Qt, but a PDF text piece is drawn with
            // its font's own spaces. So on a justified line every word is placed at the x
            // Qt gave it. (The last line of a paragraph is not justified.)
            const bool justified = paragraph.alignment == Qt::AlignJustify
                                   && li < textLayout->lineCount() - 1;

            for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
                const QTextFragment fragment = it.fragment();
                if (!fragment.isValid())
                    continue;

                const int fragmentStart = fragment.position() - block.position();
                const int from = qMax(fragmentStart, lineStart);
                const int to = qMin(fragmentStart + int(fragment.length()), lineEnd);
                if (to <= from)
                    continue;

                const QTextCharFormat format = fragment.charFormat();
                FontFamily family = FontFamily(format.property(kFontFamilyProperty).toInt());
                const QString fontName = format.property(kFontNameProperty).toString();
                if (family == FontFamily::Unknown)
                    family = classifyFont(fontName).family;
                const bool bold = format.font().bold();
                const bool italic = format.font().italic();

                // The PDF's own font if installed; the standard PDF fonts already cover Arial, Times New Roman and Courier New.
                QString systemFamily = installedFamilyFor(fontName);
                if (isStandardFamily(systemFamily))
                    systemFamily.clear();

                double sizePt = format.property(kFontSizeProperty).toDouble();
                if (sizePt <= 0.0)
                    sizePt = format.font().pixelSize() / k;

                // One piece of text on this line, from character a up to b. widthEnd is where
                // its underline stops.
                const auto addSpan = [&](int a, int b, int widthEnd) {
                    const double x0 = line.cursorToX(a);
                    const double x1 = line.cursorToX(widthEnd);

                    PlacedSpan span;
                    span.text = blockText.mid(a, b - a);
                    span.baseFont = pdfBaseFont(family, bold, italic);
                    span.sizePoints = sizePt;
                    span.color = format.foreground().color();
                    span.underlined = format.fontUnderline();
                    span.bold = bold;
                    span.italic = italic;
                    span.systemFamily = systemFamily;
                    span.origin = QPointF(region.left() + (blockPos.x() + x0) / k, baselineY);
                    span.width = (x1 - x0) / k;
                    spans.append(span);
                };

                // A tab (list marker gap) splits a piece in two; each half is placed at its own x so the gap survives.
                int pieceStart = from;
                for (int pos = from; pos <= to; ++pos) {
                    const bool atEnd = (pos == to);
                    if (!atEnd && blockText.at(pos) != QLatin1Char('\t'))
                        continue;

                    int a = pieceStart;
                    int b = pos;
                    while (a < b && blockText.at(a).isSpace())
                        ++a;
                    while (b > a && blockText.at(b - 1).isSpace())
                        --b;

                    if (b > a && !justified) {
                        addSpan(a, b, b);
                    } else if (b > a) {
                        // Split the piece into words. A word's underline runs on to the next
                        // word, so an underlined justified line stays one line.
                        QVector<int> wordStarts;
                        QVector<int> wordEnds;
                        int wordStart = a;
                        for (int i = a; i <= b; ++i) {
                            if (i < b && !blockText.at(i).isSpace())
                                continue;
                            if (i > wordStart) {
                                wordStarts.append(wordStart);
                                wordEnds.append(i);
                            }
                            wordStart = i + 1;
                        }
                        for (int w = 0; w < int(wordStarts.size()); ++w) {
                            const int widthEnd =
                                (w + 1 < int(wordStarts.size())) ? wordStarts.at(w + 1) : b;
                            addSpan(wordStarts.at(w), wordEnds.at(w), widthEnd);
                        }
                    }

                    pieceStart = pos + 1;
                }
            }
        }
    }

    return spans;
}

}