#include "MainWindow.h"
#include "PageRangeDialog.h"
#include "PdfEditor.h"
#include "ThumbnailPane.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QDialog>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressDialog>
#include <QStatusBar>

namespace {

    constexpr int kMaxImageSide = 2400;

    QByteArray encodeImageForPdf(const QImage &source)
    {
        QImage image = source;
        if (qMax(image.width(), image.height()) > kMaxImageSide)
            image = image.scaled(kMaxImageSide, kMaxImageSide, Qt::KeepAspectRatio,
                                 Qt::SmoothTransformation);

        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        if (image.hasAlphaChannel())
            image.save(&buffer, "PNG");
        else
            image.save(&buffer, "JPEG", 92);
        return bytes;
    }

    QString annotationFontName(const PageView::TextStyle &style)
    {
        const QString family = style.family.toLower();
        if (family.contains("times") || family.contains("serif"))
            return QStringLiteral("TiRo");
        if (family.contains("courier") || family.contains("mono"))
            return QStringLiteral("Cour");
        return QStringLiteral("Helv");
    }
}

QList<int> MainWindow::targetPages() const
{
    QList<int> pages = m_thumbnails->selectedPages();
    if (pages.isEmpty())
        pages << m_view->currentPage();
    return pages;
}

void MainWindow::rotateSelectedPages(int degrees)
{
    if (!m_editor)
        return;

    const QList<int> pages = targetPages();
    const int viewPage = m_view->currentPage();

    QVector<QSizeF> sizes = m_view->pageSizes();
    if (qAbs(degrees) % 180 == 90) {
        for (int page : pages) {
            if (page >= 0 && page < sizes.size())
                sizes[page].transpose();
        }
    }

    for (int page : pages) {
        if (!m_editor->rotatePage(page, degrees)) {
            QMessageBox::warning(this, "Rotate failed", m_editor->lastError());
            reopenEditor(m_history.current());
            updateActions();
            return;
        }
    }

    if (refreshAfterEdit(viewPage, pages, sizes))
        statusBar()->showMessage(QString("Rotated %1 page(s)").arg(pages.size()), 3000);
}

void MainWindow::deleteSelectedPages()
{
    if (!m_editor)
        return;

    const QList<int> pages = targetPages();
    const int pageCount = m_document->pageCount();
    if (pages.size() >= pageCount) {
        QMessageBox::information(this, "Delete pages",
                                 "A PDF must keep at least one page.");
        return;
    }

    for (int i = pages.size() - 1; i >= 0; --i) {
        if (!m_editor->deletePage(pages.at(i))) {
            QMessageBox::warning(this, "Delete failed", m_editor->lastError());
            reopenEditor(m_history.current());
            updateActions();
            return;
        }
    }

    QVector<QSizeF> sizes = m_view->pageSizes();
    for (int i = pages.size() - 1; i >= 0; --i) {
        if (pages.at(i) >= 0 && pages.at(i) < sizes.size())
            sizes.remove(pages.at(i));
    }

    const int landing = qMin(pages.first(), pageCount - pages.size() - 1);
    if (refreshAfterEdit(landing, {}, sizes))
        statusBar()->showMessage(QString("Deleted %1 page(s)").arg(pages.size()), 3000);
}

void MainWindow::movePage(int from, int to)
{
    if (!m_editor)
        return;

    if (!m_editor->movePage(from, to)) {
        QMessageBox::warning(this, "Move failed", m_editor->lastError());
        return;
    }

    QVector<QSizeF> sizes = m_view->pageSizes();
    if (from >= 0 && from < sizes.size() && to >= 0 && to < sizes.size())
        sizes.move(from, to);
    refreshAfterEdit(to, {}, sizes);
}

void MainWindow::extractPages()
{
    m_view->commitPendingEdits();
    if (!m_editor)
        return;

    const int pageCount = m_document->pageCount();
    const int current = m_view->currentPage();

    PageRangeDialog dialog(pageCount, current, this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString dest = QFileDialog::getSaveFileName(
        this, "Save extracted pages as", QString(), "PDF files (*.pdf)");
    if (dest.isEmpty())
        return;

    if (QFileInfo(dest) == QFileInfo(m_currentPath)
        || QFileInfo(dest) == QFileInfo(m_history.current())) {
        QMessageBox::warning(this, "Extract pages",
                             "Please choose a different file name than the open document.");
        return;
    }

    PdfEditor extractor;
    if (!extractor.open(m_history.current(), m_password)
        || !extractor.keepOnlyPages(dialog.firstPage(), dialog.lastPage())
        || !extractor.saveAs(dest, true, saveEncryption(), m_newPassword)) {
        QMessageBox::warning(this, "Extract failed", extractor.lastError());
        return;
    }

    statusBar()->showMessage("Extracted pages to " + QFileInfo(dest).fileName(), 4000);
}

void MainWindow::insertPages()
{
    if (!m_editor)
        return;

    const QString source = QFileDialog::getOpenFileName(
        this, "Choose the PDF to insert", QString(), "PDF files (*.pdf)");
    if (source.isEmpty())
        return;

    const int pageCount = m_document->pageCount();
    const int current = m_view->currentPage();

    bool ok = false;
    const int after = QInputDialog::getInt(
        this, "Insert pages",
        QString("Insert after page (0 = at the start, %1 = at the end):").arg(pageCount),
        current + 1, 0, pageCount, 1, &ok);
    if (!ok)
        return;

    if (!m_editor->insertPagesFrom(source, after)) {
        QMessageBox::warning(this, "Insert failed", m_editor->lastError());
        return;
    }

    refreshAfterEdit(after);
}

void MainWindow::highlightText()
{
    if (!m_editor)
        return;

    bool ok = false;
    const QString text = QInputDialog::getText(
        this, "Highlight text", "Highlight every occurrence of:",
        QLineEdit::Normal, QString(), &ok);
    if (!ok || text.trimmed().isEmpty())
        return;

    const int page = m_view->currentPage();
    QList<int> changedPages;

    const QString needle = text.trimmed();

    QProgressDialog progress(QString("Searching for \"%1\"...").arg(needle),
                             "Cancel", 0, 100, this);
    progress.setWindowTitle("Highlight text");
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(300);
    bool cancelled = false;

    const bool done = m_editor->highlightText(
        needle, &changedPages,
        [&progress, &cancelled](int pageIndex, int pageTotal) {
            progress.setMaximum(pageTotal);
            progress.setValue(pageIndex);
            if (progress.wasCanceled()) {
                cancelled = true;
                return false;
            }
            return true;
        });
    progress.reset();

    if (!done) {
        if (!cancelled)
            QMessageBox::warning(this, "Highlight failed", m_editor->lastError());
        reopenEditor(m_history.current());
        updateActions();
        if (cancelled)
            statusBar()->showMessage("Highlighting cancelled.", 4000);
        return;
    }
    if (changedPages.isEmpty()) {
        statusBar()->showMessage("No matches found.", 4000);
        return;
    }

    if (refreshAfterPageEdit(changedPages, page))
        statusBar()->showMessage(
            QString("Highlighted text on %1 page(s)").arg(changedPages.size()), 4000);
}

void MainWindow::placeText(int page, const QRectF &box, const QString &text,
                           const PageView::TextStyle &style)
{
    if (!m_editor)
        return;

    if (!m_editor->addTextBox(page, box, text, annotationFontName(style),
                              style.sizePoints, style.color.redF(),
                              style.color.greenF(), style.color.blueF())) {
        QMessageBox::warning(this, "Add text failed", m_editor->lastError());
        reopenEditor(m_history.current());
        updateActions();
        return;
    }
    refreshAfterPageEdit({ page }, page);
}

void MainWindow::updateTextAnnotation(int page, int annotationId,
                                      const QString &oldText, const QString &newText)
{
    if (!m_editor)
        return;

    if (newText == oldText)
        return;
    if (newText.trimmed().isEmpty()) {
        QMessageBox::warning(this, "Edit added text",
                             "Text cannot be empty. Use Delete added text to remove it.");
        m_view->editTextAnnotationInline(page, annotationId, oldText);
        return;
    }

    if (!m_editor->updateTextAnnotation(page, annotationId, oldText, newText)) {
        QMessageBox::warning(this, "Edit added text", m_editor->lastError());
        reopenEditor(m_history.current());
        updateActions();
        return;
    }
    refreshAfterPageEdit({ page }, page);
}

void MainWindow::deleteTextAnnotation(int page, int annotationId, const QString &text)
{
    if (!m_editor)
        return;

    if (!m_editor->deleteTextAnnotation(page, annotationId, text)) {
        QMessageBox::warning(this, "Delete added text", m_editor->lastError());
        reopenEditor(m_history.current());
        updateActions();
        return;
    }
    if (refreshAfterPageEdit({ page }, page))
        statusBar()->showMessage("Deleted added text.", 3000);
}

void MainWindow::updateTextAnnotationBounds(int page, int annotationId,
                                            const QString &text, const QRectF &bounds)
{
    if (!m_editor)
        return;

    if (!m_editor->setTextAnnotationBounds(page, annotationId, text, bounds)) {
        QMessageBox::warning(this, "Update text box failed", m_editor->lastError());
        reopenEditor(m_history.current());
        updateActions();
        return;
    }
    refreshAfterPageEdit({ page }, page);
}

void MainWindow::addImage()
{
    if (!m_editor)
        return;

    const QString path = QFileDialog::getOpenFileName(
        this, "Choose an image", QString(),
        "Images (*.png *.jpg *.jpeg *.bmp *.gif *.webp *.tif *.tiff);;All files (*)");
    if (path.isEmpty())
        return;

    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize sourceSize = reader.size();
    if (sourceSize.isValid() && qMax(sourceSize.width(), sourceSize.height()) > kMaxImageSide) {
        QSize scaledSize = sourceSize;
        scaledSize.scale(kMaxImageSide, kMaxImageSide, Qt::KeepAspectRatio);
        reader.setScaledSize(scaledSize);
    }
    const QImage image = reader.read();
    if (image.isNull()) {
        QMessageBox::warning(this, "Add image",
                             "Could not read that image: " + reader.errorString());
        return;
    }

    m_addTextAction->setChecked(false);
    m_drawAction->setChecked(false);

    m_view->beginImagePlacement(image);
    statusBar()->showMessage(
        "Click on the page where the image goes. Drag to move or resize. Use Crop to crop, "
        "Enter or a click elsewhere places it, Cancel or Esc stops placement.", 12000);
}

void MainWindow::placeImage(int page, const QRectF &box, const QImage &image)
{
    if (!m_editor)
        return;

    const QByteArray data = encodeImageForPdf(image);
    if (data.isEmpty()) {
        QMessageBox::warning(this, "Add image failed", "Could not prepare the image.");
        return;
    }

    if (!m_editor->addImageStamp(page, box, data)) {
        QMessageBox::warning(this, "Add image failed", m_editor->lastError());
        reopenEditor(m_history.current());
        updateActions();
        return;
    }
    refreshAfterPageEdit({ page }, page);
}

bool MainWindow::fillFormField(int page, const QPointF &point, PageView::FormField *result)
{
    if (!m_editor || !result)
        return false;

    PdfEditor::FormField field;
    if (!m_editor->formFieldAt(page, point, &field)) {
        QMessageBox::warning(this, "Fill form failed", m_editor->lastError());
        return true;
    }
    if (field.type == PdfEditor::FormFieldType::None)
        return false;

    result->bounds = field.bounds;
    result->value = field.value;
    result->options = field.options;
    result->optionValues = field.optionValues;
    result->checked = field.checked;
    result->readOnly = field.readOnly;
    result->multiline = field.multiline;
    result->password = field.password;
    result->editableChoice = field.editableChoice;
    result->widgetIndex = field.widgetIndex;
    if (field.readOnly) {
        const QString fieldName = field.label.isEmpty() ? field.name : field.label;
        statusBar()->showMessage(fieldName.isEmpty() ? "This form field is read-only."
                                                      : fieldName + " is read-only.",
                                 4000);
        return true;
    }

    switch (field.type) {
    case PdfEditor::FormFieldType::Text:
        result->type = PageView::FormField::Type::Text;
        return true;
    case PdfEditor::FormFieldType::ComboBox:
        result->type = PageView::FormField::Type::ComboBox;
        return true;
    case PdfEditor::FormFieldType::ListBox:
        result->type = PageView::FormField::Type::ListBox;
        return true;
    case PdfEditor::FormFieldType::CheckBox:
        result->type = PageView::FormField::Type::CheckBox;
        break;
    case PdfEditor::FormFieldType::RadioButton:
        result->type = PageView::FormField::Type::RadioButton;
        break;
    case PdfEditor::FormFieldType::None:
        return false;
    }

    if (!m_editor->setFormFieldValue(page, field.widgetIndex, QString())) {
        QMessageBox::warning(this, "Fill form failed", m_editor->lastError());
        reopenEditor(m_history.current());
        updateActions();
        return true;
    }
    refreshAfterEdit(page, {}, m_view->pageSizes());
    return true;
}

bool MainWindow::commitFormField(int page, int widgetIndex, const QString &value)
{
    if (!m_editor)
        return false;
    if (!m_editor->setFormFieldValue(page, widgetIndex, value)) {
        QMessageBox::warning(this, "Fill form failed", m_editor->lastError());
        reopenEditor(m_history.current());
        updateActions();
        return false;
    }
    return refreshAfterEdit(page, {}, m_view->pageSizes());
}

void MainWindow::placeStrokes(const QVector<PageView::Stroke> &strokes)
{
    if (!m_editor || strokes.isEmpty())
        return;

    for (const PageView::Stroke &stroke : strokes) {
        if (!m_editor->addInkStroke(stroke.page, stroke.points,
                                    stroke.style.color.redF(), stroke.style.color.greenF(),
                                    stroke.style.color.blueF(),
                                    stroke.style.widthPoints, stroke.style.opacity)) {
            QMessageBox::warning(this, "Draw failed", m_editor->lastError());
            reopenEditor(m_history.current());
            updateActions();
            return;
        }
    }

    QList<int> changedPages;
    for (const PageView::Stroke &stroke : strokes) {
        if (!changedPages.contains(stroke.page))
            changedPages.append(stroke.page);
    }
    if (refreshAfterPageEdit(changedPages, strokes.last().page))
        statusBar()->showMessage(QString("Added %1 stroke(s)").arg(strokes.size()), 3000);
}

void MainWindow::eraseInk(int page, const QVector<QPointF> &path, double radius)
{
    if (!m_editor)
        return;

    int removed = 0;
    if (!m_editor->eraseInk(page, path, radius, &removed)) {
        QMessageBox::warning(this, "Erase failed", m_editor->lastError());
        reopenEditor(m_history.current());
        updateActions();
        return;
    }
    if (removed == 0) {
        statusBar()->showMessage("No drawing strokes under the eraser.", 2500);
        return;
    }

    if (refreshAfterPageEdit({ page }, page))
        statusBar()->showMessage(
            QString("Erased %1 drawing stroke(s)").arg(removed), 3000);
}

void MainWindow::undo()
{
    if (m_view->undoLastStroke())
        return;
    stepHistory(m_history.undo());
}

void MainWindow::redo()
{
    stepHistory(m_history.redo());
}

void MainWindow::stepHistory(const QString &path)
{
    if (path.isEmpty())
        return;

    const int page = m_view->currentPage();
    if (!showFile(path, page))
        return;

    refreshFormFieldOverlays();

    updateActions();
    updateTitle();
}