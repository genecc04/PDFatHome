#include "BookmarkPane.h"
#include "PdfEditor.h"

#include <QDebug>
#include <QStandardItem>
#include <QStandardItemModel>

namespace {
constexpr int kPageRole = Qt::UserRole + 1;
constexpr int kLocationRole = Qt::UserRole + 2;

void appendBookmarks(QStandardItem *parent,
                     const QVector<PdfEditor::Bookmark> &bookmarks)
{
    for (const PdfEditor::Bookmark &bookmark : bookmarks) {
        auto *item = new QStandardItem(bookmark.title);
        item->setData(bookmark.page, kPageRole);
        item->setData(bookmark.location, kLocationRole);
        parent->appendRow(item);
        appendBookmarks(item, bookmark.children);
    }
}
}

BookmarkPane::BookmarkPane(QWidget *parent)
    : QTreeView(parent)
{
    m_model = new QStandardItemModel(this);
    setModel(m_model);
    setHeaderHidden(true);
    setUniformRowHeights(true);

    connect(this, &QTreeView::clicked, this, [this](const QModelIndex &index) {
        const int page = index.data(kPageRole).toInt();
        const QPointF location = index.data(kLocationRole).toPointF();
        emit bookmarkSelected(page, location);
    });
}

void BookmarkPane::setDocument(PdfEditor *document)
{
    m_model->clear();
    if (!document)
        return;
    const QVector<PdfEditor::Bookmark> bookmarks = document->bookmarks();
    if (!document->lastError().isEmpty())
        qWarning().noquote() << "Could not load PDF bookmarks:"
                             << document->lastError();
    appendBookmarks(m_model->invisibleRootItem(), bookmarks);
}