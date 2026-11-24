#pragma once

#include <QByteArray>
#include <QString>

namespace SystemFonts {

struct EmbeddableFont
{
    QByteArray data;
    bool syntheticBold = false;
    bool syntheticItalic = false;
};

EmbeddableFont embeddableFontFile(const QString &family, bool bold, bool italic);

}