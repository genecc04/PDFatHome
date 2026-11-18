#pragma once

#include "ReconstructedPage.h"

#include <QImage>

namespace Reconstruction {

// Both functions read pixels from any render of the same page, at any resolution
// (boxes are in points and are scaled by the image size), so they behave the same
// for scanned and digital pages.

// Run BEFORE groupParagraphs(). Finds each word's ink color and underline. On OCR
// pages (no font info) it also guesses bold from stroke thickness. groupParagraphs()
// uses this to keep a bold heading, a link and plain text apart.
void estimateLineStyles(ReconstructedPage &page, const QImage &pageImage);

// Run AFTER groupParagraphs(). Fills each paragraph's textColor and backgroundColor
// so an edit can blend in with the original.
void estimateAppearance(ReconstructedPage &page, const QImage &pageImage);

}