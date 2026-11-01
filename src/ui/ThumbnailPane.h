#pragma once
#include <QList>
#include <QListView>

class QDropEvent;
class QResizeEvent;
class PdfEditor;
class ThumbnailModel;

class ThumbnailPane : public QListView
{
    Q_OBJECT
public:
    explicit ThumbnailPane(QWidget *parent = nullptr);

    void setDocument(PdfEditor *document);
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
    void resizeEvent(QResizeEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    void updateVisiblePageRange();

    ThumbnailModel *m_model = nullptr;
    bool m_syncing = false;
};