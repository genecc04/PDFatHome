#pragma once
#include <QAbstractListModel>
#include <QCache>
#include <QQueue>
#include <QPixmap>
#include <QSet>
#include <QThreadPool>

#include "DocumentSource.h"

class QMimeData;
class QTimer;
class PdfEditor;

class ThumbnailModel : public QAbstractListModel
{
    Q_OBJECT
public:
    static constexpr int kThumbWidth = 120;
    static constexpr int kThumbHeight = 170;
    static inline const QString kPageMimeType = QStringLiteral("application/x-pdfapp-page");
    ~ThumbnailModel() override;
    void setRenderSource(const DocumentSource &source);

    explicit ThumbnailModel(QObject *parent = nullptr);

    void setDocument(PdfEditor *document);
    void setDevicePixelRatio(qreal ratio);
    void invalidatePageRenders(const QList<int> &changedPages = {});
    void setVisiblePageRange(int first, int last, int priorityPage);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;

    Qt::ItemFlags flags(const QModelIndex &index) const override;
    Qt::DropActions supportedDragActions() const override;
    Qt::DropActions supportedDropActions() const override;
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;

private:
    void dispatchRenders();
    void invalidateRenders();

    QThreadPool m_renderPool;
    DocumentSource m_renderSource;
    quint64 m_renderGeneration = 0;
    qreal m_devicePixelRatio = 1.0;
    int m_inFlight = 0;

    PdfEditor *m_document = nullptr;
    int m_pageCount = 0;
    mutable QCache<int, QPixmap> m_cache;
    mutable QQueue<int> m_renderQueue;
    mutable QSet<int> m_queuedPages;
    mutable bool m_renderQueueSaturated = false;
    int m_visibleFirst = -1;
    int m_visibleLast = -1;
    int m_priorityPage = -1;
    QTimer *m_renderTimer = nullptr;
};
