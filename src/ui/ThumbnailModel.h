#pragma once
#include <QAbstractListModel>
#include <QCache>
#include <QQueue>
#include <QPixmap>
#include <QSet>
#include <QThreadPool>

class QMimeData;
class QPdfDocument;
class QTimer;

class ThumbnailModel : public QAbstractListModel
{
    Q_OBJECT
public:
    static constexpr int kThumbWidth = 120;
    static constexpr int kThumbHeight = 170;
    static inline const QString kPageMimeType = QStringLiteral("application/x-pdfapp-page");
    ~ThumbnailModel() override;
    void setRenderSource(const QString &path, const QString &password);

    explicit ThumbnailModel(QObject *parent = nullptr);

    void setDocument(QPdfDocument *document);
    void setPageRenderDocument(QPdfDocument *document,
                               const QList<int> &changedPages = {});

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
    QString m_renderSourcePath;
    QString m_renderPassword;
    quint64 m_renderGeneration = 0;
    int m_inFlight = 0;

    QPdfDocument *m_document = nullptr;
    QPdfDocument *m_pageRenderDocument = nullptr;
    mutable QCache<int, QPixmap> m_cache;
    mutable QQueue<int> m_renderQueue;
    mutable QSet<int> m_queuedPages;
    QTimer *m_renderTimer = nullptr;
};
