#include "ReconstructionController.h"
#include "MuPdfPageProvider.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QRunnable>
#include <QThread>

#include <exception>
#include <memory>
#include <utility>

namespace {
constexpr int kMaxCachedPages = 8;

struct Worker
{
    MuPdfPageProvider provider;
    Reconstruction::ReconstructionEngine engine;
};

thread_local std::unique_ptr<Worker> t_worker;
}

ReconstructionController::ReconstructionController(QObject *parent)
    : QObject(parent)
    , m_tessdataPath(QCoreApplication::applicationDirPath() + QStringLiteral("/tessdata"))
{
    m_pool.setMaxThreadCount(1);
    m_pool.setThreadPriority(QThread::LowPriority);

    m_pool.setExpiryTimeout(-1);

    m_pool.setStackSize(16 * 1024 * 1024);
}

ReconstructionController::~ReconstructionController()
{
    ++m_generation;
    m_pool.clear();

    m_pool.start(QRunnable::create([]() { t_worker.reset(); }));
    m_pool.waitForDone();
}

void ReconstructionController::setDocument(const DocumentSource &source)
{
    ++m_generation;
    m_source = source;
    m_wanted = -1;
    m_cache.clear();
}

void ReconstructionController::requestPage(int page)
{
    m_wanted = page;
    if (page < 0)
        return;

    const auto cached = m_cache.constFind(page);
    if (cached != m_cache.cend()) {
        emit pageReady(page, cached->page, cached->source, cached->note);
        return;
    }

    startNext();
}

void ReconstructionController::startNext()
{
    if (m_running || m_source.isEmpty() || m_wanted < 0 || m_cache.contains(m_wanted))
        return;

    m_running = true;
    const quint64 generation = m_generation;
    const int page = m_wanted;
    const DocumentSource source = m_source;
    const QString tessdata = m_tessdataPath;

    m_pool.start(QRunnable::create([this, generation, page, source, tessdata]() {
        auto result = std::make_shared<Reconstruction::AutoResult>();

        qInfo("reconstruct: page %d start", page + 1);

        try {
            if (!t_worker)
                t_worker = std::make_unique<Worker>();

            QString error;
            if (!t_worker->provider.open(source, &error)) {
                result->note = error;
            } else {
                Reconstruction::AutoOptions options;
                options.tessdataPath = tessdata;
                *result = Reconstruction::reconstructPageAuto(
                    t_worker->provider, t_worker->engine, page, options);
            }
        } catch (const std::exception &e) {
            t_worker.reset();
            *result = Reconstruction::AutoResult();
            result->note = QStringLiteral("Reconstruction failed: %1")
                               .arg(QString::fromUtf8(e.what()));
        } catch (...) {
            t_worker.reset();
            *result = Reconstruction::AutoResult();
            result->note = QStringLiteral("Reconstruction failed (unknown error).");
        }

        qInfo("reconstruct: page %d done (ok=%d)", page + 1, int(result->ok));

        QMetaObject::invokeMethod(
            this,
            [this, generation, page, result]() { finish(generation, page, result); },
            Qt::QueuedConnection);
    }));
}

void ReconstructionController::finish(
    quint64 generation, int page,
    const std::shared_ptr<Reconstruction::AutoResult> &result)
{
    m_running = false;

    if (generation == m_generation) {
        if (!result->ok) {
            if (page == m_wanted)
                m_wanted = -1;
            emit failed(page, result->note);
        } else {
            if (m_cache.size() >= kMaxCachedPages)
                m_cache.clear();

            Entry entry;
            entry.page = std::make_shared<const Reconstruction::ReconstructedPage>(
                std::move(result->page));
            entry.source = int(result->source);
            entry.note = result->note;
            m_cache.insert(page, entry);
            emit pageReady(page, entry.page, entry.source, entry.note);
        }
    }

    startNext();
}