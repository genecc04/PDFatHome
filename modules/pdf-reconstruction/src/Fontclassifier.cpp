#include "reconstruction/FontClassifier.h"

#include <QRegularExpression>

#include <initializer_list>

namespace Reconstruction {

namespace {

bool containsAny(const QString &text, std::initializer_list<const char *> words)
{
    for (const char *word : words)
        if (text.contains(QLatin1String(word)))
            return true;

    return false;
}

}

FontStyle classifyFont(const QString &fontName)
{
    FontStyle style;

    const QString name = fontName.toLower();
    if (name.isEmpty())
        return style;

    // "Monotype" is a foundry, not a monospace font.
    QString monoText = name;
    monoText.remove(QLatin1String("monotype"));

    // Order matters: mono, then sans, then serif ("Century Gothic" is sans, not serif, and "Lucida Console" is mono).
    if (containsAny(monoText, {"courier", "mono", "consolas", "lucida console",
                               "menlo", "monaco", "typewriter", "cmtt", "andale"})) {
        style.family = FontFamily::Mono;
    } else if (containsAny(name, {"sans", "arial", "helvetica", "calibri", "verdana",
                                  "tahoma", "segoe", "roboto", "gothic", "trebuchet",
                                  "cmss", "nimbussan", "inter", "lato", "montserrat", "poppins",
                                  "nunito", "raleway", "manrope", "geist", "futura",
                                  "frutiger", "myriad", "avenir", "dejavusans",
                                  "liberationsans"})) {
        style.family = FontFamily::Sans;
    } else if (containsAny(name, {"times", "serif", "georgia", "garamond", "palatino",
                                  "minion", "cambria", "bookman", "century",
                                  "baskerville", "caslon", "didot", "charter", "cmr",
                                  "nimbusrom", "roman", "merriweather", "playfair", "lora",
                                  "crimson", "bodoni", "cormorant", "constantia",
                                  "perpetua", "sylfaen", "dejavuserif",
                                  "liberationserif", "tinos"})) {
        style.family = FontFamily::Serif;
    }

    static const QRegularExpression boldPattern(
        QStringLiteral("bold|black|heavy|demi|[-,_ ]bd$|semibd|extrabd"));
    static const QRegularExpression italicPattern(
        QStringLiteral("italic|oblique|ital|kursiv|slanted|[-,_]it$"));

    style.bold = name.contains(boldPattern);
    style.italic = name.contains(italicPattern);
    return style;
}

FontStyle mergeFontEvidence(const QString &fontName,
                            bool flagBold,
                            bool flagItalic,
                            bool flagSerif,
                            bool flagMonospace)
{
    FontStyle style = classifyFont(fontName);

    style.bold = style.bold || flagBold;
    style.italic = style.italic || flagItalic;

    if (flagMonospace)
        style.family = FontFamily::Mono;
    else if (style.family == FontFamily::Unknown && flagSerif)
        style.family = FontFamily::Serif;

    return style;
}

}