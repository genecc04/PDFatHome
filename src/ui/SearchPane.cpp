#include "SearchPane.h"
#include "SearchResultModel.h"

#include <QItemSelectionModel>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QVBoxLayout>

SearchPane::SearchPane(QWidget *parent)
    : QWidget(parent)
{
    m_model = new SearchResultModel(this);

    m_input = new QLineEdit(this);
    m_input->setPlaceholderText("Search in document...");
    m_input->setClearButtonEnabled(true);

    m_results = new QListView(this);
    m_results->setModel(m_model);
    m_results->setEditTriggers(QAbstractItemView::NoEditTriggers);
    connect(m_model, &SearchResultModel::searchFailed, this,
            [this](const QString &error) {
                QMessageBox::warning(this, "Search failed", error);
            });

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_input);
    layout->addWidget(m_results);

    connect(m_input, &QLineEdit::returnPressed, this, [this]() {
        m_model->setSearchString(m_input->text());
    });
    connect(m_input, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (text.isEmpty())
            m_model->setSearchString(QString());
    });

    connect(m_results->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex &current) {
                if (!current.isValid())
                    return;
                const PdfEditor::SearchResult result =
                    m_model->resultAtIndex(current.row());
                emit resultSelected(current.row(), result.page, result.location);
            });
}

void SearchPane::setDocument(PdfEditor *document, const QString &password)
{
    m_model->setDocument(document ? document->filePath() : QString(), password);
}

void SearchPane::focusSearchBox()
{
    m_input->setFocus();
    m_input->selectAll();
}