#pragma once

#include <QString>
#include <QStringList>

namespace AnnotationFonts {

struct Entry {
    const char *family;
    const char *regular;
    const char *bold;
    const char *italic;
    const char *boldItalic;
};

inline constexpr Entry kEntries[] = {
    { "Helvetica", "Helv", "HeBo", "HeOb", "HeBO" },
    { "Times",     "TiRo", "TiBo", "TiIt", "TiBI" },
    { "Courier",   "Cour", "CoBo", "CoOb", "CoBO" },
};

inline QStringList families()
{
    QStringList result;
    for (const Entry &entry : kEntries)
        result << QString::fromLatin1(entry.family);
    return result;
}

inline QString pdfName(const QString &family, bool bold = false, bool italic = false)
{
    const Entry *chosen = &kEntries[0];
    for (const Entry &entry : kEntries) {
        if (family.compare(QLatin1String(entry.family), Qt::CaseInsensitive) == 0) {
            chosen = &entry;
            break;
        }
    }

    const char *name = bold ? (italic ? chosen->boldItalic : chosen->bold)
                            : (italic ? chosen->italic : chosen->regular);
    return QString::fromLatin1(name);
}

inline QString family(const QString &name)
{
    for (const Entry &entry : kEntries) {
        if (name == QLatin1String(entry.regular) || name == QLatin1String(entry.bold)
            || name == QLatin1String(entry.italic) || name == QLatin1String(entry.boldItalic))
            return QString::fromLatin1(entry.family);
    }
    if (name.startsWith(QLatin1String("Times"), Qt::CaseInsensitive))
        return QString::fromLatin1(kEntries[1].family);
    if (name.startsWith(QLatin1String("Cour"), Qt::CaseInsensitive))
        return QString::fromLatin1(kEntries[2].family);
    return QString::fromLatin1(kEntries[0].family);
}

inline bool isBold(const QString &name)
{
    for (const Entry &entry : kEntries) {
        if (name == QLatin1String(entry.bold) || name == QLatin1String(entry.boldItalic))
            return true;
    }
    return name.contains(QLatin1String("Bold"), Qt::CaseInsensitive);
}

inline bool isItalic(const QString &name)
{
    for (const Entry &entry : kEntries) {
        if (name == QLatin1String(entry.italic) || name == QLatin1String(entry.boldItalic))
            return true;
    }
    return name.contains(QLatin1String("Italic"), Qt::CaseInsensitive)
        || name.contains(QLatin1String("Oblique"), Qt::CaseInsensitive);
}

}