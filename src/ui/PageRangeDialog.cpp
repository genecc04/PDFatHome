#include "PageRangeDialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QSpinBox>
#include <QVBoxLayout>

PageRangeDialog::PageRangeDialog(int pageCount, int currentPage, QWidget *parent,
                                 const QString &title)
    : QDialog(parent)
{
    setWindowTitle(title.isEmpty() ? QStringLiteral("Extract pages") : title);

    m_from = new QSpinBox(this);
    m_to = new QSpinBox(this);
    m_from->setRange(1, pageCount);
    m_to->setRange(1, pageCount);
    m_from->setValue(currentPage + 1);
    m_to->setValue(currentPage + 1);

    connect(m_from, &QSpinBox::valueChanged, this, [this](int value) {
        if (value > m_to->value())
            m_to->setValue(value);
    });
    connect(m_to, &QSpinBox::valueChanged, this, [this](int value) {
        if (value < m_from->value())
            m_from->setValue(value);
    });

    auto *form = new QFormLayout;
    form->addRow(QString("From page (1-%1):").arg(pageCount), m_from);
    form->addRow("To page:", m_to);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);
}

int PageRangeDialog::firstPage() const
{
    return m_from->value() - 1;
}

int PageRangeDialog::lastPage() const
{
    return m_to->value() - 1;
}