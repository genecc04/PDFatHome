#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QString>

#include <memory>

class RenderDocumentBytesCache
{
public:
    static std::shared_ptr<QByteArray> sharedBytes(const QString &path,
                                                   const QDateTime &modified,
                                                   qint64 size, QString *error)
    {
        struct Entry {
            QString path;
            QDateTime modified;
            qint64 size = -1;
            std::weak_ptr<QByteArray> bytes;
        };
        static QMutex mutex;
        static Entry entry;

        QMutexLocker locker(&mutex);
        if (entry.path == path && entry.modified == modified && entry.size == size) {
            if (std::shared_ptr<QByteArray> existing = entry.bytes.lock())
                return existing;
        }

        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            *error = QStringLiteral("Could not open PDF for background rendering: %1")
                         .arg(file.errorString());
            return nullptr;
        }
        auto bytes = std::make_shared<QByteArray>(file.readAll());
        entry.path = path;
        entry.modified = modified;
        entry.size = size;
        entry.bytes = bytes;
        return bytes;
    }
};
