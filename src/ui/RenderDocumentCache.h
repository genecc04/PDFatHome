#pragma once

#include <QBuffer>
#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QPdfDocument>
#include <QString>

#include <memory>

class RenderDocumentCache
{
public:
    QPdfDocument *document(const QString &path, const QString &password,
                           QString *error)
    {
        const QFileInfo info(path);
        const QDateTime modified = info.lastModified();
        const qint64 size = info.size();
        if (m_document && path == m_path && password == m_password
            && modified == m_modified && size == m_size)
            return m_document.get();

        reset();

        m_bytes = sharedBytes(path, modified, size, error);
        if (!m_bytes)
            return nullptr;

        m_buffer = std::make_unique<QBuffer>(m_bytes.get());
        m_buffer->open(QIODevice::ReadOnly);
        m_document = std::make_unique<QPdfDocument>();
        m_document->setPassword(password);
        m_document->load(m_buffer.get());
        if (m_document->status() != QPdfDocument::Status::Ready) {
            *error = QStringLiteral("Could not load PDF for background rendering (error %1).")
                         .arg(int(m_document->error()));
            reset();
            return nullptr;
        }

        m_path = path;
        m_password = password;
        m_modified = modified;
        m_size = size;
        return m_document.get();
    }

private:
    void reset()
    {
        m_document.reset();
        m_buffer.reset();
        m_bytes.reset();
    }

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

    // Declared in this order so they are destroyed in the reverse order.
    std::shared_ptr<QByteArray> m_bytes;
    std::unique_ptr<QBuffer> m_buffer;
    std::unique_ptr<QPdfDocument> m_document;
    QString m_path;
    QString m_password;
    QDateTime m_modified;
    qint64 m_size = -1;
};