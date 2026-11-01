#pragma once

#include <QAbstractListModel>
#include <QVector>

#include "PdfEditor.h"

class SearchResultModel : public QAbstractListModel
{
    Q_OBJECT
public:
    explicit SearchResultModel(QObject *parent = nullptr);

    void setDocument(PdfEditor *document);
    void setSearchString(const QString &text);
    QString searchString() const { return m_searchString; }
    PdfEditor::SearchResult resultAtIndex(int index) const;
    QVector<PdfEditor::SearchResult> resultsOnPage(int page) const;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;

signals:
    void searchFailed(const QString &error);

private:
    PdfEditor *m_document = nullptr;
    QString m_searchString;
    QVector<PdfEditor::SearchResult> m_results;
};
