#pragma once
#include <QList>
#include <QListView>

class QDropEvent;
class QPdfDocument;
class ThumbnailModel;

class ThumbnailPane : public QListView
{
    Q_OBJECT
public:
    explicit ThumbnailPane(QWidget *parent = nullptr);

    void setDocument(QPdfDocument *document);
    void invalidatePageRenders(const QList<int> &changedPages = {});
    void setRenderSource(const QString &path, const QString &password);
    void setCurrentPage(int page);
    void setReorderEnabled(bool enabled);

    QList<int> selectedPages() const;
    void selectPages(const QList<int> &pages);

signals:
    void pageRequested(int page);
    void pageMoveRequested(int from, int to);

protected:
    void dropEvent(QDropEvent *event) override;
    void startDrag(Qt::DropActions supportedActions) override;

private:
    ThumbnailModel *m_model = nullptr;
    bool m_syncing = false;
};