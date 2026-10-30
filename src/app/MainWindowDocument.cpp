#include "MainWindow.h"
#include "PdfEditor.h"
#include "ThumbnailPane.h"

#include <QAction>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QMimeData>
#include <QPdfDocument>
#include <QRunnable>
#include <QSettings>
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
    QVector<QVector<PdfEditor::FormField>> formFields;
    bool formFieldsLoaded = false;
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
    m_pendingPage = -1;
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

            PdfEditor probe;
            if (!probe.open(path, password)) {
                result->error = probe.lastError();
                result->passwordRejected =
                    result->error == QStringLiteral("Incorrect or missing password");
            } else if (workingPath.isEmpty() || !QFile::copy(path, workingPath)) {
                result->error = QStringLiteral("Could not create a working copy of the PDF.");
            } else {
                result->workingCopyCreated = true;
                auto editor = std::make_unique<PdfEditor>();
                if (!editor->open(workingPath, password)) {
                    result->error = editor->lastError();
                } else {
                    const int pages = editor->pageCount();
                    if (pages >= 0) {
                        result->formFields.resize(pages);
                        result->formFieldsLoaded = true;
                        for (int i = 0; i < pages; ++i) {
                            if (!editor->formFieldsOnPage(i, &result->formFields[i])) {
                                result->formFieldsLoaded = false;
                                break;
                            }
                        }
                    }
                    result->editor = std::move(editor);
                }
            }

            if (result->workingCopyCreated) {
                QPdfDocument sizeDocument;
                sizeDocument.setPassword(password);
                if (sizeDocument.load(workingPath) == QPdfDocument::Error::None
                    && sizeDocument.status() == QPdfDocument::Status::Ready) {
                    const int count = sizeDocument.pageCount();
                    result->pageSizes.reserve(count);
                    for (int i = 0; i < count; ++i)
                        result->pageSizes.append(sizeDocument.pagePointSize(i));
                }
            }

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
        m_view->setLoading(false);
        updateActions();
        updateTitle();
    };

    if (result->passwordRejected) {
        m_view->setLoading(false);
        bool ok = false;
        const QString entered = QInputDialog::getText(
            this, "Password required",
            firstAttempt ? "This PDF is protected. Enter its password:"
                         : "Incorrect password. Try again:",
            QLineEdit::Password, QString(), &ok);
        if (ok) {
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
    m_view->setPageSizeHint(result->pageSizes);
    const bool shown = showFile(working, 0, false);
    m_view->setPageSizeHint(QVector<QSizeF>());
    if (!shown) {
        m_password = previousPassword;
        if (!previousState.isEmpty() && !showFile(previousState, previousPage, false)) {
            QMessageBox::warning(
                this, "Open failed",
                "The new PDF could not be opened, and the previous PDF could not be "
                "restored. Try reopening the previous file.");
        }
        if (result->workingCopyCreated)
            removeTempFile(workingPath);
        endLoading();
        QMessageBox::warning(this, "Error", "Could not open that PDF.");
        return;
    }

    m_view->stepLoading();

    m_view->clearFormFieldOverlays();
    m_editor.reset();
    removeSnapshots(m_history.states());
    m_history.clear();
    m_currentPath = path;
    m_passwordChange = PasswordChange::None;
    m_newPassword.clear();
    m_displayName = QFileInfo(path).fileName();
    addRecentFile(path);
    m_history.reset(working);
    m_view->stepLoading();

    if (result->editor) {
        m_editor = std::move(result->editor);
        if (result->formFieldsLoaded)
            refreshFormFieldOverlays(&result->formFields);
        else
            refreshFormFieldOverlays();
    }

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
    const int pageCount = m_document->pageCount();
    if (pageCount <= 0 || page < 0 || page >= pageCount)
        return;
    m_view->jumpToPage(page);
}

bool MainWindow::showFile(const QString &path, int page, bool reportErrors)
{
    m_pendingPage = -1;
    clearPageRenderDocument();
    m_document->close();
    m_document->setPassword(m_password);

    const QPdfDocument::Error error = m_document->load(path);
    if (error != QPdfDocument::Error::None) {
        if (reportErrors) {
            QString reason;
            switch (error) {
            case QPdfDocument::Error::IncorrectPassword:
                reason = "incorrect password"; break;
            case QPdfDocument::Error::InvalidFileFormat:
                reason = "invalid file format"; break;
            case QPdfDocument::Error::UnsupportedSecurityScheme:
                reason = "unsupported security scheme"; break;
            case QPdfDocument::Error::FileNotFound:
                reason = "file not found"; break;
            default:
                reason = "unknown error"; break;
            }
            QMessageBox::warning(this, "Error",
                                 QString("Could not display the document (%1).").arg(reason));
        }
        return false;
    }
    m_view->setRenderSource(path, m_password);
    m_thumbnails->setRenderSource(path, m_password);

    const int pageCount = m_document->pageCount();
    if (pageCount <= 0) {
        m_pendingPage = qMax(0, page);
        return true;
    }
    m_pendingPage = -1;
    const int target = qBound(0, page, pageCount - 1);
    goToPage(target);
    m_thumbnails->setCurrentPage(target);
    return true;
}

void MainWindow::restorePendingPage()
{
    if (m_pendingPage < 0
        || m_document->status() != QPdfDocument::Status::Ready
        || m_document->pageCount() <= 0)
        return;

    const int target = qMin(m_pendingPage, m_document->pageCount() - 1);
    m_pendingPage = -1;
    goToPage(target);
    m_thumbnails->setCurrentPage(target);
    if (m_editor)
        refreshFormFieldOverlays();
}

void MainWindow::clearPageRenderDocument()
{
    if (m_view)
        m_view->setPageRenderDocument(nullptr);
    if (m_thumbnails)
        m_thumbnails->setPageRenderDocument(nullptr);
    if (m_pageRenderDocument)
        m_pageRenderDocument->close();
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

    {
        PdfEditor saver;
        if (!saver.open(m_history.current(), m_password)
            || !saver.saveAs(tmp, true, saveEncryption(), m_newPassword)) {
            QFile::remove(tmp);
            QMessageBox::warning(this, "Save failed", saver.lastError());
            return false;
        }
    }

    m_editor.reset();
    m_document->close();

    QString replacementMessage;
    if (!replaceFile(tmp, target, &replacementMessage)) {
        QFile::remove(tmp);
        showFile(m_history.current(), page, false);
        reopenEditor(m_history.current());
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

bool MainWindow::refreshAfterEdit(int page, const QList<int> &reselect)
{
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

    if (!showFile(newPath, page)) {
        QFile::remove(newPath);
        showFile(m_history.current(), page, false);
        reopenEditor(m_history.current());
        updateActions();
        return false;
    }

    if (!reselect.isEmpty())
        m_thumbnails->selectPages(reselect);

    removeSnapshots(m_history.push(newPath));
    if (reopenEditor(newPath))
        refreshFormFieldOverlays();
    updateActions();
    updateTitle();
    return true;
}

bool MainWindow::refreshAfterPageEdit(const QList<int> &changedPages, int page)
{
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
        return refreshAfterEdit(page);

    const QString newPath = m_tempDir.filePath(
        QString("state-%1.pdf").arg(++m_editCount));
    if (!m_editor->saveAs(newPath)) {
        QMessageBox::warning(this, "Edit failed", m_editor->lastError());
        reopenEditor(m_history.current());
        updateActions();
        return false;
    }

    m_pageRenderDocument->close();
    m_pageRenderDocument->setPassword(m_password);
    if (m_pageRenderDocument->load(newPath) != QPdfDocument::Error::None) {
        if (!showFile(newPath, page)) {
            QFile::remove(newPath);
            showFile(m_history.current(), page, false);
            reopenEditor(m_history.current());
            updateActions();
            return false;
        }
    } else {
        m_view->setPageRenderDocument(m_pageRenderDocument, pages);
        m_view->setRenderSource(newPath, m_password);
        m_thumbnails->setRenderSource(newPath, m_password);
        m_thumbnails->setPageRenderDocument(m_pageRenderDocument, pages);
    }

    removeSnapshots(m_history.push(newPath));
    m_view->refreshSelectedTextAnnotation();
    updateActions();
    updateTitle();
    return true;
}