#pragma once
#include <QList>
#include <QString>
#include <QStringList>

class EditHistory
{
public:
    // Snapshots are full file copies, so the total size is capped as well.
    static constexpr qint64 kDefaultMaxBytes = qint64(1024) * 1024 * 1024;

    explicit EditHistory(int maxUndoSteps = 20, qint64 maxBytes = kDefaultMaxBytes);

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
    QList<qint64> m_sizes;
    int m_index = -1;
    int m_saved = -1;
    int m_max;
    qint64 m_maxBytes;
};