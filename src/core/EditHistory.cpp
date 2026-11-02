#include "EditHistory.h"

EditHistory::EditHistory(int maxUndoSteps)
    : m_max(maxUndoSteps + 1)
{
}

void EditHistory::reset(const QString &originalPath)
{
    m_states = { originalPath };
    m_index = 0;
    m_saved = 0;
}

void EditHistory::clear()
{
    m_states.clear();
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

    while (m_states.size() > m_index + 1)
        dropped << m_states.takeLast();
    if (m_saved > m_index)
        m_saved = -1;

    m_states << snapshotPath;
    ++m_index;

    while (m_states.size() > m_max) {
        dropped << m_states.takeFirst();
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