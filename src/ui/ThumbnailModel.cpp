#include "ThumbnailModel.h"
#include "RenderDocumentCache.h"

#include <QDebug>
#include <QRunnable>
#include <QThread>
#include <QThreadPool>
#include <QPdfDocument>
#include <QPainter>
#include <QMimeData>
#include <QPdfDocumentRenderOptions>
#include <QTimer>

namespace {
constexpr int kMaxInFlight = 2;
}

ThumbnailModel::ThumbnailModel(QObject *parent)
    : QAbstractListModel(parent)
    , m_cache(300)
{
    m_renderPool.setMaxThreadCount(1);
    m_renderPool.setThreadPriority(QThread::LowPriority);

    m_renderTimer = new QTimer(this);
    m_renderTimer->setSingleShot(true);
    m_renderTimer->setInterval(5);
    connect(m_renderTimer, &QTimer::timeout, this, &ThumbnailModel::dispatchRenders);
}

ThumbnailModel::~ThumbnailModel()
{
    ++m_renderGeneration;
    m_renderPool.clear();
    m_renderPool.waitForDone();
}

void ThumbnailModel::setDocument(QPdfDocument *document)
{
    m_document = document;
    if (!m_document) {
        m_cache.clear();
        invalidateRenders();
        return;
    }

    connect(m_document, &QPdfDocument::statusChanged, this, [this]() {
        beginResetModel();
        m_cache.clear();
        invalidateRenders();
        endResetModel();
    });
}

void ThumbnailModel::setRenderSource(const QString &path, const QString &password)
{
    m_renderSourcePath = path;
    m_renderPassword = password;

    // Work queued against the old file is dropped. Pages that were waiting
    // get a repaint, which makes the view ask for them again.
    const QSet<int> waiting = m_queuedPages;
    invalidateRenders();
    for (int page : waiting) {
        if (page >= 0 && page < rowCount())
            emit dataChanged(index(page), index(page), { Qt::DecorationRole });
    }
}

void ThumbnailModel::setPageRenderDocument(QPdfDocument *document,
                                           const QList<int> &changedPages)
{
    m_pageRenderDocument = document;   // no longer used for rendering
    if (changedPages.isEmpty()) {
        m_cache.clear();
        invalidateRenders();
        return;
    }

    for (int page : changedPages) {
        if (page < 0 || page >= rowCount())
            continue;
        m_cache.remove(page);
        emit dataChanged(index(page), index(page), { Qt::DecorationRole });
    }
}

void ThumbnailModel::invalidateRenders()
{
    ++m_renderGeneration;
    m_renderPool.clear();
    m_inFlight = 0;
    m_renderTimer->stop();
    m_renderQueue.clear();
    m_queuedPages.clear();
}

void ThumbnailModel::dispatchRenders()
{
    if (m_renderSourcePath.isEmpty())
        return;

    while (m_inFlight < kMaxInFlight && !m_renderQueue.isEmpty()) {
        const int page = m_renderQueue.takeLast();
        if (m_cache.object(page)) {
            m_queuedPages.remove(page);
            continue;
        }

        ++m_inFlight;
        const quint64 generation = m_renderGeneration;
        const QString path = m_renderSourcePath;
        const QString password = m_renderPassword;

        m_renderPool.start(QRunnable::create(
            [this, page, generation, path, password]() {
                // One parsed document per worker thread; the pool has one thread.
                thread_local RenderDocumentCache renderDocuments;

                QImage thumbnail;
                QString error;
                if (QPdfDocument *document =
                        renderDocuments.document(path, password, &error)) {
                    if (page < 0 || page >= document->pageCount()) {
                        error = QStringLiteral("Page is out of range.");
                    } else {
                        const QSizeF pts = document->pagePointSize(page);
                        if (pts.isEmpty()) {
                            error = QStringLiteral("Page has no size.");
                        } else {
                            const qreal scale = qMin(kThumbWidth / pts.width(),
                                                     kThumbHeight / pts.height());
                            const QSize size(qMax(1, int(pts.width() * scale)),
                                             qMax(1, int(pts.height() * scale)));

                            QPdfDocumentRenderOptions options;
                            options.setRenderFlags(
                                QPdfDocumentRenderOptions::RenderFlag::Annotations);
                            const QImage rendered = document->render(page, size, options);
                            if (rendered.isNull()) {
                                error = QStringLiteral("Renderer returned an empty image.");
                            } else {
                                thumbnail = QImage(rendered.size(), QImage::Format_RGB32);
                                thumbnail.fill(Qt::white);
                                QPainter painter(&thumbnail);
                                painter.drawImage(0, 0, rendered);
                                painter.end();
                            }
                        }
                    }
                }

                QMetaObject::invokeMethod(
                    this,
                    [this, page, generation, thumbnail = std::move(thumbnail),
                     error = std::move(error)]() {
                        if (generation != m_renderGeneration)
                            return;
                        --m_inFlight;
                        m_queuedPages.remove(page);

                        if (page >= 0 && page < rowCount()) {
                            if (!error.isEmpty()) {
                                qWarning().noquote()
                                    << QStringLiteral("Could not render thumbnail %1: %2")
                                           .arg(page + 1).arg(error);
                            }
                            m_cache.insert(
                                page, new QPixmap(thumbnail.isNull()
                                                      ? QPixmap()
                                                      : QPixmap::fromImage(thumbnail)));
                            emit dataChanged(index(page), index(page),
                                             { Qt::DecorationRole });
                        }
                        dispatchRenders();
                    },
                    Qt::QueuedConnection);
            }));
    }
}

int ThumbnailModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid() || !m_document
        || m_document->status() != QPdfDocument::Status::Ready)
        return 0;
    return m_document->pageCount();
}

QVariant ThumbnailModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= rowCount())
        return {};

    const int page = index.row();

    if (role == Qt::DisplayRole)
        return QString::number(page + 1);

    if (role == Qt::DecorationRole) {
        if (QPixmap *cached = m_cache.object(page))
            return *cached;

        if (!m_queuedPages.contains(page)) {
            m_queuedPages.insert(page);
            m_renderQueue.enqueue(page);
            if (!m_renderTimer->isActive())
                m_renderTimer->start();
        }

        static const QPixmap placeholder = []() {
            QPixmap pixmap(kThumbWidth, kThumbHeight);
            pixmap.fill(Qt::white);
            QPainter painter(&pixmap);
            painter.setPen(QColor(210, 210, 210));
            painter.drawRect(pixmap.rect().adjusted(0, 0, -1, -1));
            return pixmap;
        }();
        return placeholder;
    }

    return {};
}

Qt::ItemFlags ThumbnailModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::ItemIsDropEnabled;

    return QAbstractListModel::flags(index) | Qt::ItemIsDragEnabled;
}

Qt::DropActions ThumbnailModel::supportedDragActions() const
{
    return Qt::MoveAction;
}

Qt::DropActions ThumbnailModel::supportedDropActions() const
{
    return Qt::MoveAction;
}

QStringList ThumbnailModel::mimeTypes() const
{
    return { kPageMimeType };
}

QMimeData *ThumbnailModel::mimeData(const QModelIndexList &indexes) const
{
    auto *mime = new QMimeData;
    if (!indexes.isEmpty())
        mime->setData(kPageMimeType, QByteArray::number(indexes.first().row()));
    return mime;
}