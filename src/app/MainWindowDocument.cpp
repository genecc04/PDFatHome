#include "MainWindow.h"
#include "BookmarkPane.h"
#include "PdfEditor.h"
#include "SearchPane.h"
#include "ThumbnailPane.h"

#include <QAction>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QMimeData>
#include <QRunnable>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <memory>

struct MainWindow::DocumentLoadResult
{
    std::unique_ptr<PdfEditor> editor;
    QString error;
    bool passwordRejected = false;
    bool workingCopyCreated = false;
    QVector<QSizeF> pageSizes;
};

namespace {

    constexpr int kMaxRecentFiles = 8;

    QString droppedPdf(const QMimeData *mime)
    {
        if (!mime->hasUrls())
            return QString();
        const QList<QUrl> urls = mime->urls();
        for (const QUrl &url : urls) {
            if (url.isLocalFile() && url.toLocalFile().endsWith(".pdf", Qt::CaseInsensitive))
                return url.toLocalFile();
        }
        return QString();
    }

    QString createSiblingTempPath(const QString &target, const QString &purpose,
                                  QString *error)
    {
        const QFileInfo targetInfo(target);
        const QString pattern = QDir(targetInfo.absolutePath()).filePath(
            targetInfo.fileName() + ".pdfapp-" + purpose + "-XXXXXX");
        QTemporaryFile temp(pattern);
        if (!temp.open()) {
            *error = "Could not create a temporary file beside the PDF ("
                     + temp.errorString() + ").";
            return QString();
        }

        temp.setAutoRemove(false);
        const QString path = temp.fileName();
        temp.close();
        return path;
    }

    bool replaceFile(const QString &source, const QString &target, QString *message)
    {
        const QFileInfo targetInfo(target);
        const QString backupPattern = QDir(targetInfo.absolutePath()).filePath(
            targetInfo.fileName() + ".pdfapp-backup-XXXXXX");
        QTemporaryDir backupDirectory(backupPattern);
        if (!backupDirectory.isValid()) {
            *message = "Could not create a temporary backup folder beside the PDF.";
            return false;
        }
        const QString backup = backupDirectory.filePath(targetInfo.fileName());

        QFile original(target);
        if (!original.rename(backup)) {
            *message = "Could not move the original PDF for replacement ("
                       + original.errorString() + "). Is it open in another program?";
            return false;
        }

        if (!QFile::rename(source, target)) {
            if (QFile::rename(backup, target)) {
                *message = "Could not install the saved PDF; the original was restored.";
            } else {
                backupDirectory.setAutoRemove(false);
                *message = "Could not install the saved PDF or restore the original. "
                           "Your original PDF is still available at: " + backup;
            }
            return false;
        }

        if (!QFile::remove(backup)) {
            backupDirectory.setAutoRemove(false);
            *message = "Saved, but the backup could not be removed: " + backup;
        }
        return true;
    }

}

void MainWindow::openFile()
{
    if (m_documentLoadPending)
        return;

    if (!confirmDiscard())
        return;

    const QString path = QFileDialog::getOpenFileName(
        this, "Open PDF", QString(), "PDF files (*.pdf)");
    if (path.isEmpty())
        return;

    loadDocument(path);
}

void MainWindow::openPath(const QString &path)
{
    if (m_documentLoadPending)
        return;

    if (!confirmDiscard())
        return;
    loadDocument(path);
}

void MainWindow::loadDocument(const QString &path)
{
    if (m_documentLoadPending)
        return;

    m_documentLoadPending = true;
    setBusyCursor(true);
    m_view->setLoading(true);
    updateActions();

    const QString workingPath = m_tempDir.isValid()
        ? m_tempDir.filePath(QString("state-%1.pdf").arg(++m_editCount))
        : QString();
    startDocumentLoad(path, QString(), true, workingPath);
}

void MainWindow::startDocumentLoad(const QString &path, const QString &password,
                                   bool firstAttempt, const QString &workingPath)
{
    m_documentLoadPool.start(QRunnable::create(
        [this, path, password, firstAttempt, workingPath]() {
            auto result = std::make_shared<DocumentLoadResult>();
            const auto stopIfClosing = [this, &result, &workingPath]() {
                if (!m_closing)
                    return false;
                if (result->workingCopyCreated)
                    QFile::remove(workingPath);
                return true;
            };
            if (stopIfClosing())
                return;

            bool sourceOpened = false;
            {
                PdfEditor probe;
                sourceOpened = probe.open(path, password);
                if (!sourceOpened) {
                    result->error = probe.lastError();
                    result->passwordRejected =
                        result->error == QStringLiteral("Incorrect or missing password");
                }
            }

            if (sourceOpened && stopIfClosing())
                return;
            if (sourceOpened
                && (workingPath.isEmpty() || !QFile::copy(path, workingPath))) {
                result->error = QStringLiteral("Could not create a working copy of the PDF.");
            } else if (sourceOpened) {
                result->workingCopyCreated = true;
                if (stopIfClosing())
                    return;
                auto editor = std::make_unique<PdfEditor>();
                if (!editor->open(workingPath, password)) {
                    result->error = editor->lastError();
                } else {
                    result->pageSizes = editor->pageSizes();
                    result->editor = std::move(editor);
                }
            }

            if (stopIfClosing())
                return;

            QMetaObject::invokeMethod(
                this,
                [this, path, password, firstAttempt, workingPath, result]() {
                    finishDocumentLoad(path, password, firstAttempt, workingPath, result);
                },
                Qt::QueuedConnection);
        }));
}

void MainWindow::finishDocumentLoad(const QString &path, const QString &password,
                                    bool firstAttempt, const QString &workingPath,
                                    const std::shared_ptr<DocumentLoadResult> &result)
{
    auto endLoading = [this]() {
        m_documentLoadPending = false;
        setBusyCursor(false);
        m_view->setLoading(false);
        updateActions();
        updateTitle();
    };

    if (result->passwordRejected) {
        setBusyCursor(false);
        m_view->setLoading(false);
        bool ok = false;
        const QString entered = QInputDialog::getText(
            this, "Password required",
            firstAttempt ? "This PDF is protected. Enter its password:"
                         : "Incorrect password. Try again:",
            QLineEdit::Password, QString(), &ok);
        if (ok) {
            setBusyCursor(true);
            m_view->setLoading(true);
            startDocumentLoad(path, entered, false, workingPath);
        } else {
            endLoading();
        }
        return;
    }

    const QString previousState = m_history.current();
    const int previousPage = m_view->currentPage();
    const QString previousPassword = m_password;
    const QString working = result->workingCopyCreated ? workingPath : path;

    m_password = password;
    m_view->stepLoading();
    std::unique_ptr<PdfEditor> previousEditor = std::move(m_editor);
    m_editor = std::move(result->editor);
    m_document = m_editor.get();
    if (!result->pageSizes.isEmpty())
        m_pageSizeCache.insert(working, result->pageSizes);
    m_view->setPageSizeHint(result->pageSizes);
    const bool shown = showFile(working, 0, false);
    m_view->setPageSizeHint(QVector<QSizeF>());
    if (!shown) {
        m_editor.swap(previousEditor);
        m_document = m_editor.get();
        m_password = previousPassword;
        m_formFieldPagesLoaded.clear();
        m_formFieldPagesFailed.clear();
        m_view->clearFormFieldOverlays();
        m_view->setDocument(m_document);
        m_thumbnails->setDocument(m_document);
        m_search->setDocument(m_document, m_password);
        m_bookmarks->setDocument(m_document);
        if (!previousState.isEmpty() && !showFile(previousState, previousPage, false)) {
            QMessageBox::warning(
                this, "Open failed",
                "The new PDF could not be opened, and the previous PDF could not be "
                "restored. Try reopening the previous file.");
        }
        previousEditor.reset();
        removeDeferredSnapshots();
        if (result->workingCopyCreated)
            removeTempFile(workingPath);
        endLoading();
        QMessageBox::warning(this, "Error", "Could not open that PDF.");
        return;
    }

    m_view->stepLoading();

    previousEditor.reset();
    removeDeferredSnapshots();
    removeSnapshots(m_history.states());
    m_history.clear();
    m_currentPath = path;
    m_passwordChange = PasswordChange::None;
    m_newPassword.clear();
    m_displayName = QFileInfo(path).fileName();
    addRecentFile(path);
    m_history.reset(working);
    m_view->stepLoading();

    const QString editorError = result->error;
    endLoading();

    if (!m_editor) {
        QMessageBox::warning(
            this, "Editing unavailable",
            "The PDF opened for viewing, but editing could not be started:\n\n"
                + (editorError.isEmpty()
                       ? QStringLiteral("No error details were provided.")
                       : editorError));
    }
}

void MainWindow::goToPage(int page)
{
    if (!m_document)
        return;
    const int pageCount = m_document->pageCount();
    if (pageCount <= 0 || page < 0 || page >= pageCount)
        return;
    m_view->jumpToPage(page);
}

bool MainWindow::showFile(const QString &path, int page, bool reportErrors,
                          bool preserveRasterPreviews, bool forceReload)
{
    if (!preserveRasterPreviews)
        clearPageRenderCaches();
    std::unique_ptr<PdfEditor> replacement;
    PdfEditor *document = m_editor.get();
    if (forceReload || !document || document->filePath() != path) {
        replacement = std::make_unique<PdfEditor>();
        if (!replacement->open(path, m_password)) {
            const QString error = replacement->lastError();
            if (reportErrors)
                QMessageBox::warning(this, "Error",
                                     QString("Could not display the document:\n\n%1")
                                         .arg(error));
            return false;
        }
        document = replacement.get();
    }

    const int pageCount = document->pageCount();
    if (pageCount < 0) {
        if (reportErrors) {
            QMessageBox::warning(this, "Error", document->lastError());
        }
        return false;
    }

    const auto knownSizes = m_pageSizeCache.constFind(path);
    QVector<QSizeF> pageSizes = knownSizes != m_pageSizeCache.constEnd()
                                    ? knownSizes.value() : QVector<QSizeF>();
    if (pageSizes.size() != pageCount)
        pageSizes = document->pageSizes();
    if (pageCount > 0 && pageSizes.size() != pageCount) {
        if (reportErrors) {
            const QString error = document->lastError().isEmpty()
                                      ? QStringLiteral("Could not read page dimensions.")
                                      : document->lastError();
            QMessageBox::warning(this, "Error", error);
        }
        return false;
    }

    if (replacement) {
        m_editor.swap(replacement);
        m_document = m_editor.get();
    }
    m_formFieldPagesLoaded.clear();
    m_formFieldPagesFailed.clear();
    m_view->clearFormFieldOverlays();
    m_view->setPageSizeHint(pageSizes);
    m_view->setDocument(m_document, preserveRasterPreviews);
    m_view->setPageSizeHint(QVector<QSizeF>());
    m_thumbnails->setDocument(m_document);
    m_search->setDocument(m_document, m_password);
    m_bookmarks->setDocument(m_document);
    replacement.reset();
    removeDeferredSnapshots();
    m_view->setRenderSource(path, m_password);
    m_thumbnails->setRenderSource(path, m_password);

    {
        QSignalBlocker blocker(m_pageBox);
        m_pageBox->setRange(pageCount > 0 ? 1 : 0, pageCount);
        m_pageCountLabel->setText(QString(" / %1 ").arg(pageCount));
    }
    if (pageCount <= 0) {
        return true;
    }
    const int target = qBound(0, page, pageCount - 1);

    if (isTempFile(path) && m_view->pageSizes().size() == pageCount) {
        if (m_pageSizeCache.size() > 64)
            m_pageSizeCache.clear();
        m_pageSizeCache.insert(path, m_view->pageSizes());
    }

    goToPage(target);
    m_thumbnails->setCurrentPage(target);
    return true;
}

void MainWindow::clearPageRenderCaches()
{
    if (m_view)
        m_view->invalidatePageRenders();
    if (m_thumbnails)
        m_thumbnails->invalidatePageRenders();
}

// recent files, about, drops

void MainWindow::addRecentFile(const QString &path)
{
    QSettings settings;
    QStringList recent = settings.value("recentFiles").toStringList();

    const QString full = QFileInfo(path).absoluteFilePath();
    recent.removeAll(full);
    recent.prepend(full);
    while (recent.size() > kMaxRecentFiles)
        recent.removeLast();
    settings.setValue("recentFiles", recent);

    QTimer::singleShot(0, this, &MainWindow::updateRecentMenu);
}

void MainWindow::updateRecentMenu()
{
    m_recentMenu->clear();

    QSettings settings;
    QStringList recent = settings.value("recentFiles").toStringList();
    recent.erase(std::remove_if(recent.begin(), recent.end(),
                                [](const QString &p) { return !QFileInfo::exists(p); }),
                 recent.end());
    settings.setValue("recentFiles", recent);

    m_recentMenu->setEnabled(!recent.isEmpty());
    if (recent.isEmpty())
        return;

    for (int i = 0; i < recent.size(); ++i) {
        const QString path = recent.at(i);
        QAction *action = m_recentMenu->addAction(
            QString("&%1  %2").arg(i + 1).arg(QFileInfo(path).fileName()));
        action->setStatusTip(path);
        connect(action, &QAction::triggered, this, [this, path]() { openPath(path); });
    }

    m_recentMenu->addSeparator();
    QAction *clear = m_recentMenu->addAction("&Clear list");
    connect(clear, &QAction::triggered, this, [this]() {
        QSettings().remove("recentFiles");
        QTimer::singleShot(0, this, &MainWindow::updateRecentMenu);
    });
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, "About PDF@home",
        "<h3>PDF@home</h3>"
        "<p>A desktop PDF reader and page editor built with Qt 6 and MuPDF.</p>"
        "<p>Copyright &copy; 2026 genecc04.<br>"
        "This program comes with ABSOLUTELY NO WARRANTY. It is free software, licensed "
        "under the <a href=\"https://www.gnu.org/licenses/agpl-3.0.html\">"
        "GNU Affero General Public License v3.0</a>, and you are welcome to "
        "redistribute it under its conditions.</p>"
        "<p>Source code: <a href=\"https://github.com/genecc04/offline-pdf-reader-editor\">"
        "github.com/genecc04/offline-pdf-reader-editor</a></p>"
        "<p>Uses MuPDF (Artifex Software, Inc.) and Qt. See THIRD_PARTY_NOTICES.md.</p>");
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (!droppedPdf(event->mimeData()).isEmpty())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    const QString path = droppedPdf(event->mimeData());
    if (path.isEmpty())
        return;

    event->acceptProposedAction();
    QTimer::singleShot(0, this, [this, path]() { openPath(path); });
}

// saving, editing

bool MainWindow::saveAs()
{
    m_view->commitPendingEdits();
    if (!m_editor)
        return false;

    const QString dest = QFileDialog::getSaveFileName(
        this, "Save PDF As", QString(), "PDF files (*.pdf)");
    if (dest.isEmpty())
        return false;

    if (QFileInfo(dest) == QFileInfo(m_currentPath)
        || QFileInfo(dest) == QFileInfo(m_history.current())) {
        QMessageBox::warning(this, "Save As",
                             "Please choose a different file name than the open document.");
        return false;
    }

    EditOperationGuard operation(this);
    PdfEditor saver;
    if (!saver.open(m_history.current(), m_password)
        || !saver.saveAs(dest, true, saveEncryption(), m_newPassword)) {
        QMessageBox::warning(this, "Save failed", saver.lastError());
        return false;
    }

    m_history.markSaved();
    m_passwordChange = PasswordChange::None;
    m_newPassword.clear();
    updateActions();
    updateTitle();
    statusBar()->showMessage("Saved " + QFileInfo(dest).fileName(), 4000);
    return true;
}

void MainWindow::optimizePdf()
{
    m_view->commitPendingEdits();
    if (!m_editor)
        return;

    const QString defaultName =
        QFileInfo(m_currentPath).completeBaseName() + "-optimized.pdf";
    const QString dest = QFileDialog::getSaveFileName(
        this, "Save optimized PDF copy", defaultName, "PDF files (*.pdf)");
    if (dest.isEmpty())
        return;

    if (QFileInfo(dest) == QFileInfo(m_currentPath)
        || QFileInfo(dest) == QFileInfo(m_history.current())) {
        QMessageBox::warning(this, "Optimize PDF",
                             "Choose a different file name to keep the original unchanged.");
        return;
    }

    const qint64 sourceSize = QFileInfo(m_history.current()).size();
    EditOperationGuard operation(this);
    PdfEditor optimizer;
    if (!optimizer.open(m_history.current(), m_password)
        || !optimizer.optimizeAs(dest, saveEncryption(), m_newPassword)) {
        QMessageBox::warning(this, "Optimize failed", optimizer.lastError());
        return;
    }

    const qint64 optimizedSize = QFileInfo(dest).size();
    const QString sizeSummary =
        QString("%1 MB → %2 MB")
            .arg(sourceSize / (1024.0 * 1024.0), 0, 'f', 2)
            .arg(optimizedSize / (1024.0 * 1024.0), 0, 'f', 2);
    statusBar()->showMessage(
        "Saved optimized copy: " + QFileInfo(dest).fileName() + " (" + sizeSummary + ")",
        6000);
}

void MainWindow::applyRedactions()
{
    m_view->commitPendingEdits();
    if (!m_editor || !m_view->hasRedactionAreas())
        return;

    const auto answer = QMessageBox::warning(
        this, "Apply redactions to a copy",
        "This creates a separate redacted PDF copy; the original remains unchanged and still "
        "contains the unredacted content.\n\n"
        "Content overlapping the marked areas will be removed. An intersecting image or "
        "vector drawing may be removed in full. This operation does not scrub document "
        "metadata, attachments, bookmarks, form values, or other unmarked content.\n\n"
        "Inspect the output and verify it with search and copy before sharing. Continue?",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    const QString defaultName =
        QFileInfo(m_currentPath).completeBaseName() + "-redacted.pdf";
    const QString dest = QFileDialog::getSaveFileName(
        this, "Save redacted PDF copy", defaultName, "PDF files (*.pdf)");
    if (dest.isEmpty())
        return;

    if (QFileInfo(dest) == QFileInfo(m_currentPath)
        || QFileInfo(dest) == QFileInfo(m_history.current())) {
        QMessageBox::warning(this, "Apply redactions",
                             "Choose a different file name to keep the original unchanged.");
        return;
    }

    const QVector<PdfEditor::RedactionArea> areas = m_view->redactionAreas();
    EditOperationGuard operation(this);
    PdfEditor redactor;
    if (!redactor.open(m_history.current(), m_password)
        || !redactor.redactAs(dest, areas, saveEncryption(), m_newPassword)) {
        QMessageBox::warning(this, "Redaction failed", redactor.lastError());
        return;
    }

    m_view->clearRedactionAreas();
    m_redactAction->setChecked(false);
    statusBar()->showMessage(
        "Saved redacted copy: " + QFileInfo(dest).fileName()
            + ". The original PDF was not changed; inspect the copy before sharing.",
        10000);
}

bool MainWindow::save()
{
    m_view->commitPendingEdits();
    if (!m_editor || !isDocumentModified())
        return true;

    const QString target = m_currentPath;
    if (!QFileInfo(target).isWritable()) {
        QMessageBox::warning(this, "Save",
                             "This file is read-only. Use Save As to save a copy.");
        return false;
    }

    QString tempError;
    const QString tmp = createSiblingTempPath(target, "tmp", &tempError);
    if (tmp.isEmpty()) {
        QMessageBox::warning(this, "Save failed", tempError);
        return false;
    }
    const int page = m_view->currentPage();

    EditOperationGuard operation(this);
    {
        PdfEditor saver;
        if (!saver.open(m_history.current(), m_password)
            || !saver.saveAs(tmp, true, saveEncryption(), m_newPassword)) {
            QFile::remove(tmp);
            QMessageBox::warning(this, "Save failed", saver.lastError());
            return false;
        }
    }

    m_view->setDocument(nullptr);
    m_thumbnails->setDocument(nullptr);
    m_search->setDocument(nullptr, m_password);
    m_bookmarks->setDocument(nullptr);
    m_document = nullptr;
    m_editor.reset();
    removeDeferredSnapshots();

    QString replacementMessage;
    if (!replaceFile(tmp, target, &replacementMessage)) {
        QFile::remove(tmp);
        showFile(m_history.current(), page, false);
        updateActions();
        QMessageBox::warning(this, "Save failed", replacementMessage);
        return false;
    }

    if (m_passwordChange == PasswordChange::Set)
        m_password = m_newPassword;
    else if (m_passwordChange == PasswordChange::Remove)
        m_password.clear();
    m_passwordChange = PasswordChange::None;
    m_newPassword.clear();

    removeSnapshots(m_history.states());
    openWorkingCopy(target, page);
    updateActions();
    updateTitle();
    statusBar()->showMessage(
        replacementMessage.isEmpty() ? "Saved " + m_displayName : replacementMessage,
        replacementMessage.isEmpty() ? 4000 : 10000);
    return true;
}

bool MainWindow::refreshAfterEdit(int page, const QList<int> &reselect,
                                  const QVector<QSizeF> &pageSizes,
                                  bool preserveRasterPreviews)
{
    EditOperationGuard operation(this);

    if (!m_tempDir.isValid()) {
        QMessageBox::warning(this, "Edit failed", "Could not create a temporary folder.");
        reopenEditor(m_history.current());
        updateActions();
        return false;
    }

    const QString newPath = m_tempDir.filePath(
        QString("state-%1.pdf").arg(++m_editCount));
    if (!m_editor->saveAs(newPath)) {
        QMessageBox::warning(this, "Edit failed", m_editor->lastError());
        reopenEditor(m_history.current());
        updateActions();
        return false;
    }

    if (!pageSizes.isEmpty())
        m_pageSizeCache.insert(newPath, pageSizes);

    if (!showFile(newPath, page, true, preserveRasterPreviews)) {
        QFile::remove(newPath);
        showFile(m_history.current(), page, false);
        updateActions();
        return false;
    }

    if (!reselect.isEmpty())
        m_thumbnails->selectPages(reselect);

    removeSnapshots(m_history.push(newPath));
    refreshFormFieldOverlays();
    updateActions();
    updateTitle();
    return true;
}

bool MainWindow::refreshAfterPageEdit(const QList<int> &changedPages, int page)
{
    EditOperationGuard operation(this);

    if (!m_tempDir.isValid()) {
        QMessageBox::warning(this, "Edit failed", "Could not create a temporary folder.");
        reopenEditor(m_history.current());
        updateActions();
        return false;
    }

    QList<int> pages;
    for (int changedPage : changedPages) {
        if (changedPage >= 0 && changedPage < m_document->pageCount()
            && !pages.contains(changedPage))
            pages.append(changedPage);
    }
    if (pages.isEmpty())
        return refreshAfterEdit(page, {}, m_view->pageSizes());

    const QString newPath = m_tempDir.filePath(
        QString("state-%1.pdf").arg(++m_editCount));
    if (!m_editor->saveAs(newPath)) {
        QMessageBox::warning(this, "Edit failed", m_editor->lastError());
        reopenEditor(m_history.current());
        updateActions();
        return false;
    }

    m_view->invalidatePageRenders(pages);
    m_view->setRenderSource(newPath, m_password);
    m_thumbnails->setRenderSource(newPath, m_password);
    m_thumbnails->invalidatePageRenders(pages);

    if (m_view->pageSizes().size() == m_document->pageCount())
        m_pageSizeCache.insert(newPath, m_view->pageSizes());

    removeSnapshots(m_history.push(newPath));
    for (int changedPage : pages) {
        m_formFieldPagesLoaded.remove(changedPage);
        m_formFieldPagesFailed.remove(changedPage);
        m_view->setFormFields(changedPage, {});
    }
    m_view->refreshSelectedTextAnnotation();
    refreshFormFieldOverlays();
    updateActions();
    updateTitle();
    return true;
}