#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QThreadPool>
#include <QVector>

#include <atomic>
#include <memory>

#include "PdfEditor.h"

class SearchResultModel : public QAbstractListModel
{
    Q_OBJECT
public:
    explicit SearchResultModel(QObject *parent = nullptr);
    ~SearchResultModel() override;

    void setDocument(const QString &path, const QString &password);
    void setSearchString(const QString &text);
    QString searchString() const { return m_searchString; }
    PdfEditor::SearchResult resultAtIndex(int index) const;
    const QVector<PdfEditor::SearchResult> &resultsOnPage(int page) const;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;

signals:
    void searchFailed(const QString &error);

private:
    QThreadPool m_searchPool;
    std::shared_ptr<std::atomic_bool> m_searchCancellation;
    quint64 m_searchGeneration = 0;
    QString m_documentPath;
    QString m_password;
    QString m_searchString;
    QVector<PdfEditor::SearchResult> m_results;
    QHash<int, QVector<PdfEditor::SearchResult>> m_resultsByPage;
};
