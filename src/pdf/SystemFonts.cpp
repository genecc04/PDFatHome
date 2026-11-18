#include "SystemFonts.h"

#include <QtGlobal>

#include <cstddef>
#include <cstring>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#pragma comment(lib, "gdi32.lib")
#endif

namespace SystemFonts {

namespace {

struct SfntInfo
{
    bool plainTrueType = false;
    bool variable = false;
    bool hasOs2 = false;
    int fsType = 0;
    int weightClass = 400;
    bool italic = false;
};

inline unsigned be16(const unsigned char *p)
{
    return (unsigned(p[0]) << 8) | unsigned(p[1]);
}

inline unsigned long be32(const unsigned char *p)
{
    return (static_cast<unsigned long>(p[0]) << 24) | (static_cast<unsigned long>(p[1]) << 16)
         | (static_cast<unsigned long>(p[2]) << 8) | static_cast<unsigned long>(p[3]);
}

inline SfntInfo readSfntInfo(const unsigned char *data, size_t size)
{
    SfntInfo info;
    if (size < 12)
        return info;

    const bool trueType = (data[0] == 0 && data[1] == 1 && data[2] == 0 && data[3] == 0)
                          || std::memcmp(data, "true", 4) == 0;
    if (!trueType)
        return info;

    const unsigned tableCount = be16(data + 4);
    if (size < 12 + size_t(tableCount) * 16)
        return info;
    info.plainTrueType = true;

    for (unsigned i = 0; i < tableCount; ++i) {
        const unsigned char *entry = data + 12 + 16 * i;
        const unsigned long offset = be32(entry + 8);
        const unsigned long length = be32(entry + 12);

        if (std::memcmp(entry, "fvar", 4) == 0) {
            info.variable = true;
        } else if (std::memcmp(entry, "OS/2", 4) == 0 && length >= 68
                   && offset + length <= size) {
            const unsigned char *table = data + offset;
            info.hasOs2 = true;
            info.weightClass = int(be16(table + 4));
            info.fsType = int(be16(table + 8));
            info.italic = (be16(table + 62) & 1) != 0;
        }
    }
    return info;
}

}

#ifdef Q_OS_WIN

namespace {

QByteArray readFontFile(HDC dc, const QString &family, bool bold, bool italic)
{
    wchar_t actual[LF_FACESIZE] = {};
    if (GetTextFaceW(dc, LF_FACESIZE, actual) == 0
        || QString::fromWCharArray(actual).compare(family, Qt::CaseInsensitive) != 0)
        return {};

    const DWORD size = GetFontData(dc, 0, 0, nullptr, 0);
    if (size == GDI_ERROR || size < 1024 || size > 32u * 1024u * 1024u)
        return {};

    QByteArray data(int(size), Qt::Uninitialized);
    if (GetFontData(dc, 0, 0, data.data(), size) != size)
        return {};

    const SfntInfo info = readSfntInfo(reinterpret_cast<const unsigned char *>(data.constData()),
                                       size_t(data.size()));
    if (!info.plainTrueType || info.variable || !info.hasOs2)
        return {};
    if (info.fsType & 0x0002)
        return {};
    if (info.fsType & 0x0200)
        return {};
    if (bold && info.weightClass < 600)
        return {};
    if (italic && !info.italic)
        return {};

    return data;
}

}

QByteArray embeddableFontFile(const QString &family, bool bold, bool italic)
{
    if (family.isEmpty() || family.size() >= LF_FACESIZE)
        return {};

    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc)
        return {};

    LOGFONTW wanted = {};
    wanted.lfHeight = -100;
    wanted.lfWeight = bold ? FW_BOLD : FW_NORMAL;
    wanted.lfItalic = italic ? TRUE : FALSE;
    wanted.lfCharSet = DEFAULT_CHARSET;
    wanted.lfOutPrecision = OUT_TT_ONLY_PRECIS;
    family.toWCharArray(wanted.lfFaceName);

    QByteArray result;
    HFONT font = CreateFontIndirectW(&wanted);
    HGDIOBJ previous = font ? SelectObject(dc, font) : nullptr;
    if (previous)
        result = readFontFile(dc, family, bold, italic);

    if (previous)
        SelectObject(dc, previous);
    if (font)
        DeleteObject(font);
    DeleteDC(dc);
    return result;
}

#else

QByteArray embeddableFontFile(const QString &, bool, bool)
{
    return {};
}

#endif

}