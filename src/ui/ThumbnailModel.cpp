#include "ThumbnailModel.h"
#include "MuPdfRenderCache.h"
#include "PdfEditor.h"

#include <QDebug>
#include <QRunnable>
#include <QThread>
#include <QThreadPool>
#include <QPainter>
#include <QMimeData>
#include <QLoggingCategory>
#include <QTimer>

namespace {
Q_LOGGING_CATEGORY(lcThumbnailCache, "pdfapp.cache.thumbnails")

constexpr int kMaxInFlight = 2;
constexpr int kMaxQueuedRenders = 32;
constexpr int kThumbnailCacheKilobytes = 12 * 1024;
constexpr int kThumbnailPrefetchRadius = 4;
}

ThumbnailModel::ThumbnailModel(QObject *parent)
    : QAbstractListModel(parent)
    , m_cache(kThumbnailCacheKilobytes)
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

void ThumbnailModel::setDevicePixelRatio(qreal ratio)
{
    ratio = qBound(1.0, ratio, 4.0);
    if (qFuzzyCompare(ratio, m_devicePixelRatio))
        return;
    m_devicePixelRatio = ratio;
    // Keep roughly the same number of thumbnails cached at higher densities.
    m_cache.setMaxCost(int(kThumbnailCacheKilobytes * ratio * ratio));
    m_cache.clear();
    invalidateRenders();
    if (rowCount() > 0)
        emit dataChanged(index(0), index(rowCount() - 1), { Qt::DecorationRole });
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

void ThumbnailModel::setVisiblePageRange(int first, int last, int priorityPage)
{
    const int count = rowCount();
    if (count <= 0 || first < 0 || last < first) {
        m_visibleFirst = -1;
        m_visibleLast = -1;
        m_priorityPage = -1;
        return;
    }

    m_visibleFirst = qBound(0, first, count - 1);
    m_visibleLast = qBound(m_visibleFirst, last, count - 1);
    m_priorityPage = qBound(m_visibleFirst, priorityPage, m_visibleLast);

    const int keepFirst = qMax(0, m_visibleFirst - kThumbnailPrefetchRadius);
    const int keepLast = qMin(count - 1, m_visibleLast + kThumbnailPrefetchRadius);
    QQueue<int> retained;
    while (!m_renderQueue.isEmpty()) {
        const int page = m_renderQueue.dequeue();
        if (page >= keepFirst && page <= keepLast) {
            retained.enqueue(page);
        } else {
            m_queuedPages.remove(page);
        }
    }
    m_renderQueue.swap(retained);

    if (!m_renderQueue.isEmpty() && !m_renderTimer->isActive())
        m_renderTimer->start();
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
        int nextIndex = 0;
        if (m_visibleFirst >= 0) {
            auto score = [this](int page) {
                const bool visible = page >= m_visibleFirst && page <= m_visibleLast;
                return qMakePair(visible ? 0 : 1, qAbs(page - m_priorityPage));
            };
            for (int i = 1; i < m_renderQueue.size(); ++i) {
                if (score(m_renderQueue.at(i)) < score(m_renderQueue.at(nextIndex)))
                    nextIndex = i;
            }
        }
        const int page = m_renderQueue.takeAt(nextIndex);
        if (m_cache.object(page)) {
            m_queuedPages.remove(page);
            continue;
        }

        ++m_inFlight;
        const quint64 generation = m_renderGeneration;
        const QString path = m_renderSourcePath;
        const QString password = m_renderPassword;
        const qreal dpr = m_devicePixelRatio;

        m_renderPool.start(QRunnable::create(
            [this, page, generation, path, password, dpr]() {
                // One MuPDF document per worker thread; the pool has one thread.
                thread_local MuPdfRenderCache renderer;

                QImage thumbnail;
                QString error;
                if (renderer.open(path, password, &error)) {
                    QImage rendered = renderer.renderThumbnail(
                        page, QSizeF(kThumbWidth * dpr, kThumbHeight * dpr), &error);
                    if (rendered.isNull()) {
                        if (error.isEmpty())
                            error = QStringLiteral("Renderer returned an empty image.");
                    } else {
                        rendered.setDevicePixelRatio(dpr);
                        thumbnail = std::move(rendered);
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
                                                      : QPixmap::fromImage(thumbnail)),
                                qMax(1, int((qint64(thumbnail.width())
                                             * thumbnail.height() * 4 + 1023)
                                            / 1024)));
                            qCDebug(lcThumbnailCache)
                                << "Thumbnail cache bytes:"
                                << qint64(m_cache.totalCost()) * 1024
                                << "images:" << m_cache.size();
                            emit dataChanged(index(page), index(page),
                                             { Qt::DecorationRole });
                        }
                        dispatchRenders();
                        if (m_renderQueueSaturated && rowCount() > 0) {
                            m_renderQueueSaturated = false;
                            const int first = m_visibleFirst >= 0 ? m_visibleFirst : 0;
                            const int last = m_visibleLast >= first
                                                 ? m_visibleLast : rowCount() - 1;
                            emit dataChanged(index(first), index(last),
                                             {Qt::DecorationRole});
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