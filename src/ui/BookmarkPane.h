#pragma once
#include <QPointF>
#include <QTreeView>

class QStandardItemModel;
class PdfEditor;

class BookmarkPane : public QTreeView
{
    Q_OBJECT
public:
    explicit BookmarkPane(QWidget *parent = nullptr);

    void setDocument(PdfEditor *document);

signals:
    void bookmarkSelected(int page, QPointF location);

private:
    QStandardItemModel *m_model = nullptr;
};