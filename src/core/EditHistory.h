#pragma once
#include <QString>
#include <QStringList>

class EditHistory
{
public:
    explicit EditHistory(int maxUndoSteps = 20);

    void reset(const QString &originalPath);
    void clear();

    QString current() const;
    QStringList states() const { return m_states; }
    bool canUndo() const;
    bool canRedo() const;
    bool isModified() const;

    QStringList push(const QString &snapshotPath);

    QString undo();
    QString redo();
    QString peekUndo() const;
    QString peekRedo() const;
    void markSaved();

private:
    QStringList m_states;
    int m_index = -1;
    int m_saved = -1;
    int m_max;
};