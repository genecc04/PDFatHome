#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QThreadPool>

#include <memory>

#include "DocumentSource.h"
#include "reconstruction/AutoReconstruct.h"

class ReconstructionController : public QObject
{
    Q_OBJECT
public:
    explicit ReconstructionController(QObject *parent = nullptr);
    ~ReconstructionController() override;

    void setDocument(const DocumentSource &source);

    void requestPage(int page);

signals:
    void pageReady(int page,
                   std::shared_ptr<const Reconstruction::ReconstructedPage> result,
                   int source,
                   const QString &note);
    void failed(int page, const QString &error);

private:
    struct Entry
    {
        std::shared_ptr<const Reconstruction::ReconstructedPage> page;
        int source = 0;
        QString note;
    };

    void startNext();
    void finish(quint64 generation, int page,
                const std::shared_ptr<Reconstruction::AutoResult> &result);

    QThreadPool m_pool;
    DocumentSource m_source;
    QString m_tessdataPath;
    quint64 m_generation = 0;
    bool m_running = false;
    int m_wanted = -1;
    QHash<int, Entry> m_cache;
};