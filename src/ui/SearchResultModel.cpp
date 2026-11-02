#include "SearchResultModel.h"

#include <QMetaObject>
#include <QRunnable>

#include <utility>

SearchResultModel::SearchResultModel(QObject *parent)
    : QAbstractListModel(parent)
{
    m_searchPool.setMaxThreadCount(1);
}

SearchResultModel::~SearchResultModel()
{
    if (m_searchCancellation)
        m_searchCancellation->store(true);
    m_searchPool.clear();
    m_searchPool.waitForDone();
}

void SearchResultModel::setDocument(const DocumentSource &source)
{
    m_source = source;
    setSearchString(m_searchString);
}

void SearchResultModel::setSearchString(const QString &text)
{
    ++m_searchGeneration;
    if (m_searchCancellation)
        m_searchCancellation->store(true);
    m_searchPool.clear();

    beginResetModel();
    m_searchString = text.trimmed();
    m_results.clear();
    m_resultsByPage.clear();
    endResetModel();

    if (m_source.isEmpty() || m_searchString.isEmpty())
        return;

    const quint64 generation = m_searchGeneration;
    const QString query = m_searchString;
    const DocumentSource source = m_source;
    auto cancellation = std::make_shared<std::atomic_bool>(false);
    m_searchCancellation = cancellation;

    m_searchPool.start(QRunnable::create(
        [this, generation, query, source, cancellation]() {
            PdfEditor editor;
            QString error;
            QVector<PdfEditor::SearchResult> results;
            if (!editor.open(source.path, source.password)) {
                error = editor.lastError();
            } else {
                results = editor.search(query, [cancellation]() {
                    return cancellation->load();
                });
                if (!cancellation->load())
                    error = editor.lastError();
            }

            if (cancellation->load())
                return;

            QMetaObject::invokeMethod(
                this,
                [this, generation, results = std::move(results),
                 error = std::move(error)]() {
                    if (generation != m_searchGeneration)
                        return;

                    beginResetModel();
                    m_results = std::move(results);
                    m_resultsByPage.clear();
                    for (const PdfEditor::SearchResult &result : m_results)
                        m_resultsByPage[result.page].append(result);
                    endResetModel();

                    if (!error.isEmpty())
                        emit searchFailed(error);
                },
                Qt::QueuedConnection);
        }));
}

PdfEditor::SearchResult SearchResultModel::resultAtIndex(int index) const
{
    return index >= 0 && index < m_results.size()
               ? m_results.at(index) : PdfEditor::SearchResult();
}

const QVector<PdfEditor::SearchResult> &SearchResultModel::resultsOnPage(int page) const
{
    static const QVector<PdfEditor::SearchResult> emptyResults;
    const auto results = m_resultsByPage.constFind(page);
    return results == m_resultsByPage.cend() ? emptyResults : results.value();
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
