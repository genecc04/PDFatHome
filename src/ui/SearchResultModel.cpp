#include "SearchResultModel.h"

SearchResultModel::SearchResultModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

void SearchResultModel::setDocument(PdfEditor *document)
{
    m_document = document;
    setSearchString(m_searchString);
}

void SearchResultModel::setSearchString(const QString &text)
{
    beginResetModel();
    m_searchString = text.trimmed();
    QString error;
    m_results.clear();
    if (m_document && !m_searchString.isEmpty()) {
        m_results = m_document->search(m_searchString);
        error = m_document->lastError();
    }
    endResetModel();
    if (!error.isEmpty())
        emit searchFailed(error);
}

PdfEditor::SearchResult SearchResultModel::resultAtIndex(int index) const
{
    return index >= 0 && index < m_results.size()
               ? m_results.at(index) : PdfEditor::SearchResult();
}

QVector<PdfEditor::SearchResult> SearchResultModel::resultsOnPage(int page) const
{
    QVector<PdfEditor::SearchResult> results;
    for (const PdfEditor::SearchResult &result : m_results) {
        if (result.page == page)
            results.append(result);
    }
    return results;
}

int SearchResultModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_results.size();
}

QVariant SearchResultModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_results.size())
        return {};
    if (role != Qt::DisplayRole)
        return {};

    const PdfEditor::SearchResult &result = m_results.at(index.row());
    return QStringLiteral("p.%1   ...%2[%3]%4...")
        .arg(result.page + 1)
        .arg(result.contextBefore, m_searchString, result.contextAfter);
}
