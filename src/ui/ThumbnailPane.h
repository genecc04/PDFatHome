#pragma once
#include <QList>
#include <QListView>

#include "DocumentSource.h"

class QDropEvent;
class QEvent;
class QResizeEvent;
class QShowEvent;
class PdfEditor;
class ThumbnailModel;

class ThumbnailPane : public QListView
{
    Q_OBJECT
public:
    explicit ThumbnailPane(QWidget *parent = nullptr);

    void setDocument(PdfEditor *document);
    void invalidatePageRenders(const QList<int> &changedPages = {});
    void setRenderSource(const DocumentSource &source);
    void setCurrentPage(int page);
    void setReorderEnabled(bool enabled);

    QList<int> selectedPages() const;
    void selectPages(const QList<int> &pages);

signals:
    void pageRequested(int page);
    void pageMoveRequested(int from, int to);

protected:
    bool event(QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void startDrag(Qt::DropActions supportedActions) override;
    void resizeEvent(QResizeEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    void updateVisiblePageRange();

    ThumbnailModel *m_model = nullptr;
    bool m_syncing = false;
};