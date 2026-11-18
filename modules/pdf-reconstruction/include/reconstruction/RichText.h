#pragma once

#include "ReconstructedPage.h"

#include <QColor>
#include <QFont>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QVector>

#include <memory>

namespace Reconstruction {

// The original look of each piece of text rides along on its character format,
// because Qt only knows the substitute font it displays. This is what lets an
// edit keep the PDF's font name, size and family.
inline const int kFontNameProperty = QTextFormat::UserProperty + 1;
inline const int kFontSizeProperty = QTextFormat::UserProperty + 2;   // points
inline const int kFontFamilyProperty = QTextFormat::UserProperty + 3; // FontFamily as int

// The Qt font closest to a PDF font: the PDF's own font if installed, else
// Arial / Times New Roman / Courier New (metric-compatible with Helvetica /
// Times / Courier).
QFont qtFont(const QString &pdfFontName, FontFamily family, bool bold, bool italic,
             double pixelSize);

// The installed family a PDF font name stands for, or empty: "ABCDEF+Inter-Regular"
// finds "Inter", "ArialMT" finds "Arial". GUI thread only.
QString installedFamilyFor(const QString &pdfFontName);

QTextCharFormat charFormat(const TextRun &run, double pixelsPerPoint);

// A paragraph's runs; one run in the paragraph's overall style if it has none.
QVector<TextRun> runsOf(const TextParagraph &paragraph);

// Baseline-to-baseline distance (points): the paragraph's own spacing, grown in
// proportion when text is bigger than the original, so lines never overlap.
double lineSpacingFor(const TextParagraph &paragraph, double biggestSizePoints);

// Rich text for a paragraph: each run keeps its font, size, color and underline,
// with the original line spacing and alignment. widthPx is the layout width in
// pixels; pixelsPerPoint the display scale.
std::unique_ptr<QTextDocument> makeDocument(const TextParagraph &paragraph,
                                            double pixelsPerPoint, double widthPx);

// Pixels from the top of the document to the first line's baseline.
double firstBaselineOffset(const QTextDocument *document);

// Reads the (possibly edited) text back into runs. Typed text inherits the style next to it.
QVector<TextRun> runsFromDocument(const QTextDocument *document, double pixelsPerPoint);

// A piece of text with an exact position, ready to be written into a PDF.
struct PlacedSpan
{
    QString text;
    QString baseFont;        // standard PDF font: "Helvetica-Bold", "Times-Italic", ...
    double sizePoints = 12.0;
    QColor color = QColor(Qt::black);
    bool underlined = false;
    bool bold = false;
    bool italic = false;
    // The PDF's own font, when installed and not already covered by a standard PDF
    // font (Arial, Times New Roman and Courier New are). Empty = use baseFont.
    QString systemFamily;
    QPointF origin;          // left end of the baseline, page points, top-left origin
    double width = 0.0;      // points
};

QString pdfBaseFont(FontFamily family, bool bold, bool italic);

// Lays the paragraph out as the editor shows it and returns each line split into
// same-style pieces, each with its own x. Placing every piece absolutely keeps
// small screen-vs-PDF font width differences from adding up along a line.
//   region         where the text is laid out (see editRegion())
//   firstBaselineY where the first baseline sits (page points)
QVector<PlacedSpan> placeSpans(const TextParagraph &paragraph, const QRectF &region,
                               double firstBaselineY);

}