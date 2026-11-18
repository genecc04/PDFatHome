#pragma once

#include <QByteArray>
#include <QString>

namespace SystemFonts {

QByteArray embeddableFontFile(const QString &family, bool bold, bool italic);

}