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
#include <QMimeData>
#include <QPdfDocument>
#include <QSettings>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTimer>
#include <QUrl>
#include <algorithm>

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
    if (!confirmDiscard())
        return;
    loadDocument(path);
}

void MainWindow::loadDocument(const QString &path)
{
    m_pendingPage = -1;
    const QString previousState = m_history.current();
    const int previousPage = m_view->currentPage();
    auto restorePreviousDocument = [this, &previousState, previousPage]() {
        if (previousState.isEmpty())
            return;
        if (!showFile(previousState, previousPage, false)) {
            QMessageBox::warning(
                this, "Open failed",
                "The new PDF could not be opened, and the previous PDF could not be "
                "restored. Try reopening the previous file.");
        }
    };

    QString usedPassword;
    auto authenticatedEditor = std::make_unique<PdfEditor>();
    bool editorAvailable = authenticatedEditor->open(path);
    QString editorOpenError = editorAvailable ? QString() : authenticatedEditor->lastError();
    if (!editorAvailable && editorOpenError == QStringLiteral("Incorrect or missing password")) {
        bool firstAttempt = true;
        while (!editorAvailable) {
            const QString prompt = firstAttempt
                ? "This PDF is protected. Enter its password:"
                : "Incorrect password. Try again:";
            firstAttempt = false;

            bool ok = false;
            const QString password = QInputDialog::getText(
                this, "Password required", prompt, QLineEdit::Password, QString(), &ok);
            if (!ok) {
                restorePreviousDocument();
                updateTitle();
                return;
            }

            auto candidateEditor = std::make_unique<PdfEditor>();
            if (candidateEditor->open(path, password)) {
                authenticatedEditor = std::move(candidateEditor);
                editorAvailable = true;
                usedPassword = password;
            } else {
                editorOpenError = candidateEditor->lastError();
                editorAvailable = false;
                if (editorOpenError != QStringLiteral("Incorrect or missing password"))
                    break;
            }
        }
    }

    clearPageRenderDocument();
    m_document->close();
    m_document->setPassword(usedPassword);
    QPdfDocument::Error error = m_document->load(path);

    if (error != QPdfDocument::Error::None) {
        restorePreviousDocument();
        updateTitle();
        QMessageBox::warning(this, "Error", "Could not open that PDF.");
        return;
    }

    m_view->clearFormFieldOverlays();
    m_editor.reset();
    removeSnapshots(m_history.states());
    m_history.clear();
    m_currentPath = path;
    m_password = usedPassword;
    m_view->setRenderSource(path, m_password);
    m_thumbnails->setRenderSource(path, m_password);
    m_passwordChange = PasswordChange::None;
    m_newPassword.clear();
    m_displayName = QFileInfo(path).fileName();
    addRecentFile(path);

    QString working = path;
    bool workingCopyCreated = false;
    if (m_tempDir.isValid()) {
        const QString copy = m_tempDir.filePath(
            QString("state-%1.pdf").arg(++m_editCount));
        if (QFile::copy(path, copy)) {
            working = copy;
            workingCopyCreated = true;
        }
    }
    m_history.reset(working);
    if (editorAvailable) {
        authenticatedEditor.reset();
        if (workingCopyCreated) {
            if (!showFile(working, 0)) {
                editorAvailable = false;
                editorOpenError = "Could not load the PDF working copy for display.";
                showFile(path, 0, false);
            } else {
                m_editor = std::make_unique<PdfEditor>();
                editorAvailable = m_editor->open(working, m_password);
                if (editorAvailable)
                    refreshFormFieldOverlays();
                else
                    editorOpenError = m_editor->lastError();
            }
        } else {
            editorAvailable = false;
            editorOpenError = "Could not create a working copy of the PDF.";
        }
    }
    if (!editorAvailable) {
        m_editor.reset();
        QMessageBox::warning(
            this, "Editing unavailable",
            "The PDF opened for viewing, but editing could not be started:\n\n"
                + (editorOpenError.isEmpty()
                       ? QStringLiteral("No error details were provided.")
                       : editorOpenError));
    }

    updateActions();
    updateTitle();
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
