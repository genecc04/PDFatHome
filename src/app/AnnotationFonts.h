#pragma once

#include <QString>
#include <QStringList>

// The fonts the text tool offers, as the family shown in the UI and the PDF
// base-14 font name stored in the annotation.
namespace AnnotationFonts {

struct Entry {
    const char *family;
    const char *pdfName;
};

inline constexpr Entry kEntries[] = {
    { "Helvetica", "Helv" },
    { "Times", "TiRo" },
    { "Courier", "Cour" },
};

inline QStringList families()
{
    QStringList result;
    for (const Entry &entry : kEntries)
        result << QString::fromLatin1(entry.family);
    return result;
}

// Unknown input falls back to the first entry (Helvetica).
inline QString pdfName(const QString &family)
{
    for (const Entry &entry : kEntries) {
        if (family.compare(QLatin1String(entry.family), Qt::CaseInsensitive) == 0)
            return QString::fromLatin1(entry.pdfName);
    }
    return QString::fromLatin1(kEntries[0].pdfName);
}

inline QString family(const QString &pdfName)
{
    for (const Entry &entry : kEntries) {
        if (pdfName == QLatin1String(entry.pdfName))
            return QString::fromLatin1(entry.family);
    }
    return QString::fromLatin1(kEntries[0].family);
}

}
