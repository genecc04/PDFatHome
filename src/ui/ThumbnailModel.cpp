#include "ThumbnailModel.h"
#include "MuPdfRenderCache.h"
#include "PdfEditor.h"

#include <QDebug>
#include <QRunnable>
#include <QThread>
#include <QThreadPool>
#include <QPainter>
#include <QMimeData>
#include <QTimer>

namespace {
constexpr int kMaxInFlight = 2;
constexpr int kMaxQueuedRenders = 32;
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

void ThumbnailModel::setDocument(PdfEditor *document)
{
    beginResetModel();
    m_document = document;
    m_pageCount = m_document ? qMax(0, m_document->pageCount()) : 0;
    m_cache.clear();
    invalidateRenders();
    endResetModel();
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

void ThumbnailModel::invalidatePageRenders(const QList<int> &changedPages)
{
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
    m_renderQueueSaturated = false;
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
                // One MuPDF document per worker thread; the pool has one thread.
                thread_local MuPdfRenderCache renderer;

                QImage thumbnail;
                QString error;
                if (renderer.open(path, password, &error)) {
                    const QSizeF pts = renderer.pagePointSize(page, &error);
                    if (pts.isEmpty()) {
                        if (error.isEmpty())
                            error = QStringLiteral("Page has no size.");
                    } else {
                        const qreal scale = qMin(kThumbWidth / pts.width(),
                                                 kThumbHeight / pts.height());
                        const QSize size(qMax(1, int(pts.width() * scale)),
                                         qMax(1, int(pts.height() * scale)));

                        const QImage rendered = renderer.render(page, size, true, &error);
                        if (rendered.isNull()) {
                            if (error.isEmpty())
                                error = QStringLiteral("Renderer returned an empty image.");
                        } else {
                            // The page is already drawn on white, so this only
                            // changes the pixel format.
                            thumbnail = rendered.convertToFormat(QImage::Format_RGB32);
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
                        if (m_renderQueueSaturated && rowCount() > 0) {
                            m_renderQueueSaturated = false;
                            emit dataChanged(index(0), index(rowCount() - 1),
                                             { Qt::DecorationRole });
                        }
                    },
                    Qt::QueuedConnection);
            }));
    }
}

int ThumbnailModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid() || !m_document)
        return 0;
    return m_pageCount;
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
            if (m_renderQueue.size() < kMaxQueuedRenders) {
                m_queuedPages.insert(page);
                m_renderQueue.enqueue(page);
                if (!m_renderTimer->isActive())
                    m_renderTimer->start();
            } else {
                m_renderQueueSaturated = true;
            }
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