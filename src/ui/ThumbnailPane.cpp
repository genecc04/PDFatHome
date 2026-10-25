#include "ThumbnailPane.h"
#include "ThumbnailModel.h"

#include <QDropEvent>
#include <QItemSelectionModel>
#include <QMimeData>

#include <algorithm>

ThumbnailPane::ThumbnailPane(QWidget *parent)
    : QListView(parent)
{
    m_model = new ThumbnailModel(this);
    setModel(m_model);

    setViewMode(QListView::ListMode);
    setIconSize(QSize(ThumbnailModel::kThumbWidth, ThumbnailModel::kThumbHeight));
    setSpacing(8);
    setUniformItemSizes(true);

    setSelectionMode(QAbstractItemView::ExtendedSelection);

    setDragDropMode(QAbstractItemView::NoDragDrop);
    setDropIndicatorShown(true);
    setDefaultDropAction(Qt::MoveAction);

    connect(selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex &current) {
                if (m_syncing || !current.isValid())
                    return;
                emit pageRequested(current.row());
            });
}

void ThumbnailPane::setDocument(QPdfDocument *document)
{
    m_model->setDocument(document);
}

void ThumbnailPane::setPageRenderDocument(QPdfDocument *document,
                                          const QList<int> &changedPages)
{
    m_model->setPageRenderDocument(document, changedPages);
}

void ThumbnailPane::setRenderSource(const QString &path, const QString &password)
{
    m_model->setRenderSource(path, password);
}

void ThumbnailPane::setCurrentPage(int page)
{
    const QModelIndex index = m_model->index(page);
    if (!index.isValid())
        return;

    m_syncing = true;

    const bool alreadySelected = selectionModel()->isSelected(index);
    selectionModel()->setCurrentIndex(index,
        alreadySelected ? QItemSelectionModel::NoUpdate
                        : QItemSelectionModel::ClearAndSelect);
    scrollTo(index);

    m_syncing = false;
}

void ThumbnailPane::setReorderEnabled(bool enabled)
{
    setDragDropMode(enabled ? QAbstractItemView::InternalMove
                            : QAbstractItemView::NoDragDrop);
}

QList<int> ThumbnailPane::selectedPages() const
{
    QList<int> pages;
    const QModelIndexList indexes = selectionModel()->selectedIndexes();
    for (const QModelIndex &index : indexes)
        pages << index.row();
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    return pages;
}

void ThumbnailPane::selectPages(const QList<int> &pages)
{
    selectionModel()->clearSelection();
    for (int page : pages) {
        const QModelIndex index = m_model->index(page);
        if (index.isValid())
            selectionModel()->select(index, QItemSelectionModel::Select);
    }
}

void ThumbnailPane::startDrag(Qt::DropActions supportedActions)
{
    if (selectionModel()->selectedIndexes().size() != 1)
        return;
    QListView::startDrag(supportedActions);
}

void ThumbnailPane::dropEvent(QDropEvent *event)
{
    if (!event->mimeData()->hasFormat(ThumbnailModel::kPageMimeType)) {
        event->ignore();
        return;
    }

    const int from = event->mimeData()->data(ThumbnailModel::kPageMimeType).toInt();
    const int count = m_model->rowCount();
    const int y = event->position().toPoint().y();

    int insertBefore = count;
    for (int row = 0; row < count; ++row) {
        if (y < visualRect(m_model->index(row)).center().y()) {
            insertBefore = row;
            break;
        }
    }

    const int to = (insertBefore > from) ? insertBefore - 1 : insertBefore;

    event->acceptProposedAction();
    if (to != from)
        emit pageMoveRequested(from, to);
}