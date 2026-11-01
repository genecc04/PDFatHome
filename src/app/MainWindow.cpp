#include "MainWindow.h"
#include "BookmarkPane.h"
#include "PdfEditor.h"
#include "SearchPane.h"
#include "ThumbnailPane.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDebug>
#include <QDir>
#include <QDockWidget>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QSettings>
#include <QSpinBox>
#include <QStatusBar>
#include <QTimer>

MainWindow::EditOperationGuard::EditOperationGuard(MainWindow *window)
    : m_window(window)
{
    m_window->beginEditOperation();
}

MainWindow::EditOperationGuard::~EditOperationGuard()
{
    m_window->endEditOperation();
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    m_documentLoadPool.setMaxThreadCount(1);

    setupViewer();
    setupThumbnailPane();
    setupSearchPane();
    setupBookmarkPane();
    setupMenus();
    setupToolbar();
    setupImageToolbar();
    setupTextToolbar();
    setupDrawToolbar();
    updateActions();

    setAcceptDrops(true);
    setWindowIcon(qApp->windowIcon());
    setWindowTitle("PDF@home");
    resize(900, 700);

    QSettings settings;
    restoreGeometry(settings.value("window/geometry").toByteArray());
    restoreState(settings.value("window/state").toByteArray(), 1);
}

MainWindow::~MainWindow()
{
    setBusyCursor(false);
    m_documentLoadPool.waitForDone();   // the worker holds `this`; let it finish first
    if (m_view) {
        m_view->invalidatePageRenders();
        m_view->setDocument(nullptr);
    }
    if (m_thumbnails) {
        m_thumbnails->invalidatePageRenders();
        m_thumbnails->setDocument(nullptr);
    }
    if (m_searchDock) {
        delete m_searchDock;
        m_searchDock = nullptr;
        m_search = nullptr;
    }
    if (m_bookmarks)
        m_bookmarks->setDocument(nullptr);

    m_document = nullptr;
    m_editor.reset();
    removeDeferredSnapshots();

    if (!m_tempDir.isValid())
        return;

    const QFileInfoList files = QDir(m_tempDir.path()).entryInfoList(
        QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
    for (const QFileInfo &fileInfo : files)
        removeTempFile(fileInfo.absoluteFilePath(), true);
}

// document state

bool MainWindow::confirmDiscard()
{
    m_view->commitPendingEdits();
    if (!isDocumentModified())
        return true;

    const auto answer = QMessageBox::question(
        this, "Unsaved changes",
        "You have unsaved changes. Save them first?",
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);

    if (answer == QMessageBox::Save)
        return save();
    return answer == QMessageBox::Discard;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!confirmDiscard()) {
        event->ignore();
        return;
    }

    QSettings settings;
    settings.setValue("window/geometry", saveGeometry());
    settings.setValue("window/state", saveState(1));
    m_closing = true;
    event->accept();
}

void MainWindow::updateTitle()
{
    if (m_displayName.isEmpty())
        setWindowTitle("PDF@home");
    else
        setWindowTitle(m_displayName + (isDocumentModified() ? " *" : "") + " - PDF@home");
}

void MainWindow::updateActions()
{
    const bool editable = (m_editor != nullptr) && !m_documentLoadPending
                          && !m_documentOperationPending;

    m_saveAction->setEnabled(editable && isDocumentModified());
    m_saveAsAction->setEnabled(editable);
    m_optimizeAction->setEnabled(editable);
    m_setPasswordAction->setEnabled(editable);
    m_removePasswordAction->setEnabled(editable && isPasswordProtected());
    m_extractAction->setEnabled(editable);
    m_exportPngAction->setEnabled(editable);
    m_exportSvgAction->setEnabled(editable);
    m_insertAction->setEnabled(editable);
    m_highlightAction->setEnabled(editable);
    m_addTextAction->setEnabled(editable);
    if ((!m_editor || m_documentLoadPending) && m_addTextAction->isChecked())
        m_addTextAction->setChecked(false);
    m_rotateLeftAction->setEnabled(editable);
    m_rotateRightAction->setEnabled(editable);
    m_deletePageAction->setEnabled(editable);
    m_thumbnails->setReorderEnabled(editable);

    m_undoAction->setEnabled(editable && (m_history.canUndo() || m_view->hasPendingStrokes()));
    m_redoAction->setEnabled(editable && m_history.canRedo());
    m_addImageAction->setEnabled(editable);
    m_cropImageAction->setEnabled(editable && m_view->hasPositionedImage());
    m_cancelImageAction->setEnabled(editable && m_view->hasActiveImagePlacement());
    m_deleteTextAction->setEnabled(editable && m_view->hasSelectedTextAnnotation());
    m_deleteImageAction->setEnabled(editable && m_view->hasSelectedImageStamp());

    m_drawAction->setEnabled(editable);
    if ((!m_editor || m_documentLoadPending) && m_drawAction->isChecked())
        m_drawAction->setChecked(false);
}

void MainWindow::setBusyCursor(bool busy)
{
    if (busy == m_busyCursorActive)
        return;

    if (busy)
        QApplication::setOverrideCursor(Qt::WaitCursor);
    else
        QApplication::restoreOverrideCursor();
    m_busyCursorActive = busy;
}

void MainWindow::beginEditOperation()
{
    ++m_editOperationDepth;
    if (m_editOperationDepth != 1)
        return;

    ++m_editOperationGeneration;
    m_documentOperationPending = true;
    m_view->setInteractionBlocked(true);
    setBusyCursor(true);
    updateActions();
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

void MainWindow::endEditOperation()
{
    if (m_editOperationDepth <= 0)
        return;
    if (--m_editOperationDepth != 0)
        return;

    const quint64 generation = ++m_editOperationGeneration;
    QTimer::singleShot(150, this, [this, generation]() {
        if (generation != m_editOperationGeneration || m_editOperationDepth != 0)
            return;

        m_documentOperationPending = false;
        m_view->setInteractionBlocked(false);
        setBusyCursor(false);
        updateActions();
        updateTitle();
    });
}

bool MainWindow::reopenEditor(const QString &path)
{
    m_formFieldPagesLoaded.clear();
    m_formFieldPagesFailed.clear();
    if (m_view)
        m_view->clearFormFieldOverlays();

    auto editor = std::make_unique<PdfEditor>();
    if (!editor->open(path, m_password)) {
        if (m_view)
            m_view->setDocument(nullptr);
        if (m_thumbnails)
            m_thumbnails->setDocument(nullptr);
        if (m_search)
            m_search->setDocument(nullptr, m_password);
        if (m_bookmarks)
            m_bookmarks->setDocument(nullptr);
        m_document = nullptr;
        m_editor.reset();
        removeDeferredSnapshots();
        return false;
    }
    m_editor.swap(editor);
    m_document = m_editor.get();
    if (m_view)
        m_view->setDocument(m_document);
    if (m_thumbnails)
        m_thumbnails->setDocument(m_document);
    if (m_search)
        m_search->setDocument(m_document, m_password);
    if (m_bookmarks)
        m_bookmarks->setDocument(m_document);
    editor.reset();
    removeDeferredSnapshots();
    return true;
}

void MainWindow::removeSnapshots(const QStringList &paths)
{
    for (const QString &path : paths) {
        m_pageSizeCache.remove(path);
        if (!isTempFile(path))
            continue;

        const bool editorHasOpenSnapshot =
            m_editor && QDir::cleanPath(m_editor->filePath()).compare(
                            QDir::cleanPath(path), Qt::CaseInsensitive) == 0;
        if ((editorHasOpenSnapshot || !removeTempFile(path))
            && !m_deferredSnapshotRemovals.contains(path)) {
            m_deferredSnapshotRemovals.append(path);
        }
    }
    removeDeferredSnapshots();
}

void MainWindow::removeDeferredSnapshots()
{
    for (auto it = m_deferredSnapshotRemovals.begin();
         it != m_deferredSnapshotRemovals.end();) {
        const bool editorHasOpenSnapshot =
            m_editor && QDir::cleanPath(m_editor->filePath()).compare(
                            QDir::cleanPath(*it), Qt::CaseInsensitive) == 0;
        if (editorHasOpenSnapshot) {
            ++it;
            continue;
        }

        if (removeTempFile(*it, true))
            it = m_deferredSnapshotRemovals.erase(it);
        else
            ++it;
    }
}

bool MainWindow::removeTempFile(const QString &path, bool reportFailure)
{
    QFile file(path);
    if (!file.exists())
        return true;

    if (!file.setPermissions(file.permissions() | QFileDevice::WriteOwner)
        || !file.remove()) {
        if (reportFailure) {
            qWarning().noquote() << "Could not remove temporary PDF snapshot"
                                 << path << ":" << file.errorString();
        }
        return false;
    }
    return true;
}

bool MainWindow::isTempFile(const QString &path) const
{
    if (!m_tempDir.isValid())
        return false;
    const QString folder = QDir::cleanPath(m_tempDir.path()) + '/';
    return QDir::cleanPath(path).startsWith(folder);
}

bool MainWindow::isDocumentModified() const
{
    return m_history.isModified() || m_view->hasPendingChanges()
           || m_passwordChange != PasswordChange::None;
}

bool MainWindow::isPasswordProtected() const
{
    return m_passwordChange == PasswordChange::Set
           || (m_passwordChange == PasswordChange::None && !m_password.isEmpty());
}

PdfEditor::Encryption MainWindow::saveEncryption() const
{
    switch (m_passwordChange) {
    case PasswordChange::Set:    return PdfEditor::Encryption::Aes256;
    case PasswordChange::Remove: return PdfEditor::Encryption::Remove;
    default:                     return PdfEditor::Encryption::Keep;
    }
}

void MainWindow::setDocumentPassword()
{
    if (!m_editor)
        return;

    bool ok = false;
    const QString first = QInputDialog::getText(
        this, "Set password",
        "Enter a password. Anyone opening the saved file will need it.\n"
        "If you forget it, the file cannot be recovered.",
        QLineEdit::Password, QString(), &ok);
    if (!ok)
        return;
    if (first.isEmpty()) {
        QMessageBox::warning(this, "Set password", "The password cannot be empty.");
        return;
    }
    if (first.toUtf8().size() > 127) {
        QMessageBox::warning(this, "Set password",
                             "That password is too long (127 characters at most).");
        return;
    }

    const QString second = QInputDialog::getText(
        this, "Set password", "Enter the password again to confirm:",
        QLineEdit::Password, QString(), &ok);
    if (!ok)
        return;
    if (second != first) {
        QMessageBox::warning(this, "Set password", "The two passwords don't match.");
        return;
    }

    m_passwordChange = PasswordChange::Set;
    m_newPassword = first;
    updateActions();
    updateTitle();
    statusBar()->showMessage(
        "The password will be applied when you save (File > Save or Save As).", 8000);
}

void MainWindow::removeDocumentPassword()
{
    if (!m_editor)
        return;

    const auto answer = QMessageBox::question(
        this, "Remove password",
        "Remove the password? Anyone will be able to open the saved file without one.",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    m_passwordChange = m_password.isEmpty() ? PasswordChange::None : PasswordChange::Remove;
    m_newPassword.clear();
    updateActions();
    updateTitle();
    statusBar()->showMessage(m_passwordChange == PasswordChange::Remove
        ? "The password will be removed when you save (File > Save or Save As)."
        : "No password will be set.", 8000);
}

bool MainWindow::openWorkingCopy(const QString &source, int page)
{
    QString working = source;
    if (m_tempDir.isValid()) {
        const QString copy = m_tempDir.filePath(QString("state-%1.pdf").arg(++m_editCount));
        if (QFile::copy(source, copy))
            working = copy;
    }

    m_history.reset(working);
    const bool shown = showFile(working, page);
    if (!shown)
        return shown;

    refreshFormFieldOverlays();
    return shown;
}

bool MainWindow::refreshFormFieldOverlays(int firstPage, int lastPage)
{
    if (!m_editor || !m_document)
        return false;

    const int pageCount = m_document->pageCount();
    if (pageCount <= 0)
        return true;

    if (firstPage < 0 || lastPage < firstPage) {
        const QPair<int, int> visibleRange = m_view->visiblePageRange();
        firstPage = visibleRange.first;
        lastPage = visibleRange.second;
    }
    if (firstPage < 0 || lastPage < firstPage)
        return true;

    constexpr int prefetchPages = 1;
    const int keepFirstPage = qMax(0, firstPage - prefetchPages);
    const int keepLastPage = qMin(pageCount - 1, lastPage + prefetchPages);
    QSet<int> pagesToKeep;

    for (int pageIndex = keepFirstPage; pageIndex <= keepLastPage; ++pageIndex) {
        pagesToKeep.insert(pageIndex);
        if (m_formFieldPagesLoaded.contains(pageIndex)
            || m_formFieldPagesFailed.contains(pageIndex))
            continue;

        QVector<PdfEditor::FormField> editorFields;
        if (!m_editor->formFieldsOnPage(pageIndex, &editorFields)) {
            m_formFieldPagesFailed.insert(pageIndex);
            QMessageBox::warning(this, "Form fields",
                                 "Could not load form fields for display: "
                                     + m_editor->lastError());
            return false;
        }

        QVector<PageView::FormField> viewFields;
        viewFields.reserve(editorFields.size());
        for (const PdfEditor::FormField &editorField : editorFields) {
            PageView::FormField viewField;
            switch (editorField.type) {
            case PdfEditor::FormFieldType::Text:
                viewField.type = PageView::FormField::Type::Text;
                break;
            case PdfEditor::FormFieldType::CheckBox:
                viewField.type = PageView::FormField::Type::CheckBox;
                break;
            case PdfEditor::FormFieldType::RadioButton:
                viewField.type = PageView::FormField::Type::RadioButton;
                break;
            case PdfEditor::FormFieldType::ComboBox:
                viewField.type = PageView::FormField::Type::ComboBox;
                break;
            case PdfEditor::FormFieldType::ListBox:
                viewField.type = PageView::FormField::Type::ListBox;
                break;
            case PdfEditor::FormFieldType::None:
                continue;
            }
            viewField.bounds = editorField.bounds;
            viewField.value = editorField.value;
            viewField.options = editorField.options;
            viewField.optionValues = editorField.optionValues;
            viewField.checked = editorField.checked;
            viewField.readOnly = editorField.readOnly;
            viewField.multiline = editorField.multiline;
            viewField.password = editorField.password;
            viewField.editableChoice = editorField.editableChoice;
            viewField.widgetIndex = editorField.widgetIndex;
            viewFields.append(viewField);
        }
        m_view->setFormFields(pageIndex, viewFields);
        m_formFieldPagesLoaded.insert(pageIndex);
    }

    const QSet<int> loadedPages = m_formFieldPagesLoaded;
    for (int pageIndex : loadedPages) {
        if (pagesToKeep.contains(pageIndex))
            continue;
        m_view->setFormFields(pageIndex, {});
        m_formFieldPagesLoaded.remove(pageIndex);
    }
    return true;
}