#pragma once

#include "ReconstructedPage.h"

#include <QString>

namespace Reconstruction {

struct FontStyle
{
    FontFamily family = FontFamily::Unknown;
    bool bold = false;
    bool italic = false;
};

// Guesses sans/serif/mono, bold and italic from a font NAME ("ABCDEF+Arial-BoldMT"
// is fine, subset tag or not). Unknown names give FontFamily::Unknown, not a guess.
FontStyle classifyFont(const QString &fontName);

// Combines a name with flags from a PDF library or Tesseract. Bold/italic if
// either says so. The monospace flag wins; otherwise the name decides the family,
// and the serif flag is used only when the name says nothing.
FontStyle mergeFontEvidence(const QString &fontName,
                            bool flagBold,
                            bool flagItalic,
                            bool flagSerif,
                            bool flagMonospace);

}