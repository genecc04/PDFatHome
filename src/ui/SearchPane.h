#pragma once
#include <QPointF>
#include <QWidget>

class QLineEdit;
class QListView;
class PdfEditor;
class SearchResultModel;

class SearchPane : public QWidget
{
    Q_OBJECT
public:
    explicit SearchPane(QWidget *parent = nullptr);

    void setDocument(PdfEditor *document);
    SearchResultModel *searchModel() const { return m_model; }
    void focusSearchBox();

signals:
    void resultSelected(int index, int page, QPointF location);

private:
    QLineEdit *m_input = nullptr;
    QListView *m_results = nullptr;
    SearchResultModel *m_model = nullptr;
};