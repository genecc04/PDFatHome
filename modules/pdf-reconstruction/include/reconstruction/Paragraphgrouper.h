#pragma once

#include "ReconstructedPage.h"

namespace Reconstruction {

// Tunable rules for deciding that a line continues the paragraph above it.
struct ParagraphOptions
{
    // Max baseline-to-baseline distance, in font sizes. Normal leading is  ~1.2-1.5; a paragraph gap pushes past ~1.8.
    double maxLineSpacing = 1.7;

    // Max font-size difference between two lines, as a fraction of the bigger size. Separates headings from body text.
    double maxSizeChange = 0.2;

    // Max jump in the lines' left edges, in font sizes. Catches another column or list item.
    double maxLeftShift = 4.0;

    // Max difference between two lines' ink colors (sum of the R, G, B
    // differences, 0..765). Separates links and gray notes from body text.
    int maxColorChange = 200;

    // A gap between two words wider than this many font sizes splits the line
    // (0 = off). Normal word spaces are ~0.25; 4 only catches real gaps such as
    // table cells or a logo's text on the same row.
    double maxWordGap = 4.0;

    // A new Tesseract/MuPDF block always starts a new paragraph.
    bool respectBlocks = true;

    // Keep the original line breaks as hard breaks, so editing one line never
    // re-wraps (and moves) the lines after it. If false, running text is joined
    // and re-wrapped.
    bool keepLineBreaks = true;
};

// Splits lines at gaps wider than maxGapInFontSizes font sizes; each piece keeps
// its words, colors and baseline and can be edited alone. Call it BEFORE
// estimateLineStyles() and groupParagraphs(). It invalidates LineRefs, so never
// call it on a page whose paragraphs are already built. Calling it twice is safe.
void splitLinesAtGaps(ReconstructedPage &page, double maxGapInFontSizes = 4.0);

// Fills page.paragraphs from page.blocks. Call after coordinates are in points (all the rules above are measured in points).
void groupParagraphs(ReconstructedPage &page,
                     const ParagraphOptions &options = ParagraphOptions());

}