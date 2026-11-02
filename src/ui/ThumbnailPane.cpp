#include "ThumbnailPane.h"
#include "ThumbnailModel.h"

#include <QDropEvent>
#include <QItemSelectionModel>
#include <QMimeData>
#include <QResizeEvent>
#include <QScrollBar>
#include <QTimer>

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
    connect(verticalScrollBar(), &QScrollBar::valueChanged,
            this, &ThumbnailPane::updateVisiblePageRange);
    connect(m_model, &QAbstractItemModel::modelReset, this, [this]() {
        QTimer::singleShot(0, this, &ThumbnailPane::updateVisiblePageRange);
    });
}

void ThumbnailPane::setDocument(PdfEditor *document)
{
    m_model->setDocument(document);
    QTimer::singleShot(0, this, &ThumbnailPane::updateVisiblePageRange);
}

void ThumbnailPane::invalidatePageRenders(const QList<int> &changedPages)
{
    m_model->invalidatePageRenders(changedPages);
}

void ThumbnailPane::setRenderSource(const DocumentSource &source)
{
    m_model->setRenderSource(source);
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
    updateVisiblePageRange();
}

void ThumbnailPane::updateVisiblePageRange()
{
    if (!m_model || m_model->rowCount() <= 0 || viewport()->height() <= 0) {
        if (m_model)
            m_model->setVisiblePageRange(-1, -1, -1);
        return;
    }

    const int centerX = viewport()->width() / 2;
    const auto rowAtEdge = [this, centerX](int edge, int direction) {
        const int limit = qMin(64, viewport()->height());
        for (int offset = 0; offset <= limit; ++offset) {
            const int y = edge + offset * direction;
            if (y < 0 || y >= viewport()->height())
                break;
            const QModelIndex item = indexAt(QPoint(centerX, y));
            if (item.isValid())
                return item.row();
        }
        return -1;
    };

    const int first = rowAtEdge(0, 1);
    const int last = rowAtEdge(viewport()->height() - 1, -1);
    if (first >= 0 && last >= first)
        m_model->setVisiblePageRange(first, last, currentIndex().row());
    else
        m_model->setVisiblePageRange(-1, -1, -1);
}

void ThumbnailPane::resizeEvent(QResizeEvent *event)
{
    QListView::resizeEvent(event);
    updateVisiblePageRange();
}

bool ThumbnailPane::event(QEvent *event)
{
    const bool handled = QListView::event(event);
    if (event->type() == QEvent::DevicePixelRatioChange)
        m_model->setDevicePixelRatio(devicePixelRatioF());
    return handled;
}

void ThumbnailPane::showEvent(QShowEvent *event)
{
    QListView::showEvent(event);
    m_model->setDevicePixelRatio(devicePixelRatioF());
}

void ThumbnailPane::scrollContentsBy(int dx, int dy)
{
    QListView::scrollContentsBy(dx, dy);
    updateVisiblePageRange();
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
    QItemSelection selection;
    for (int page : pages) {
        const QModelIndex index = m_model->index(page);
        if (index.isValid())
            selection.select(index, index);
    }
    if (!selection.isEmpty())
        selectionModel()->select(selection, QItemSelectionModel::Select);
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