#include "EditHistory.h"

#include <QFileInfo>

EditHistory::EditHistory(int maxUndoSteps, qint64 maxBytes)
    : m_max(maxUndoSteps + 1)
    , m_maxBytes(maxBytes)
{
}

void EditHistory::reset(const QString &originalPath)
{
    m_states = { originalPath };
    m_sizes = { QFileInfo(originalPath).size() };
    m_index = 0;
    m_saved = 0;
}

void EditHistory::clear()
{
    m_states.clear();
    m_sizes.clear();
    m_index = -1;
    m_saved = -1;
}

QString EditHistory::current() const
{
    return m_index >= 0 ? m_states.at(m_index) : QString();
}

bool EditHistory::canUndo() const
{
    return m_index > 0;
}

bool EditHistory::canRedo() const
{
    return m_index >= 0 && m_index + 1 < m_states.size();
}

bool EditHistory::isModified() const
{
    return m_index != m_saved;
}

QStringList EditHistory::push(const QString &snapshotPath)
{
    QStringList dropped;

    while (m_states.size() > m_index + 1) {
        dropped << m_states.takeLast();
        m_sizes.removeLast();
    }
    if (m_saved > m_index)
        m_saved = -1;

    m_states << snapshotPath;
    m_sizes << QFileInfo(snapshotPath).size();
    ++m_index;

    qint64 totalBytes = 0;
    for (qint64 size : std::as_const(m_sizes))
        totalBytes += qMax<qint64>(0, size);

    // Always keep the current state and one step of undo, however large.
    while (m_states.size() > m_max
           || (m_maxBytes > 0 && totalBytes > m_maxBytes && m_states.size() > 2)) {
        dropped << m_states.takeFirst();
        totalBytes -= qMax<qint64>(0, m_sizes.takeFirst());
        --m_index;
        if (m_saved >= 0)
            --m_saved;
    }
    return dropped;
}

QString EditHistory::undo()
{
    if (!canUndo())
        return QString();
    --m_index;
    return m_states.at(m_index);
}

QString EditHistory::redo()
{
    if (!canRedo())
        return QString();
    ++m_index;
    return m_states.at(m_index);
}

QString EditHistory::peekUndo() const
{
    return canUndo() ? m_states.at(m_index - 1) : QString();
}

QString EditHistory::peekRedo() const
{
    return canRedo() ? m_states.at(m_index + 1) : QString();
}

void EditHistory::markSaved()
{
    m_saved = m_index;
}