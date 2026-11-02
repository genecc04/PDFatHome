#include "MainWindow.h"
#include "ColorMenuButton.h"
#include "PageRangeDialog.h"
#include "PdfEditor.h"
#include "ThumbnailPane.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QImage>
#include <QImageReader>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QMessageBox>
#include <QPainter>
#include <QPen>
#include <QProgressDialog>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStatusBar>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

namespace {

    constexpr int kMaxImageSide = 2400;
    constexpr QSize kSignatureImageSize(1200, 360);

    QString signatureStorageDirectory(QString *error)
    {
        const QString appDataPath =
            QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (appDataPath.isEmpty()) {
            *error = "Could not locate the application's local data folder.";
            return {};
        }

        const QString path = QDir(appDataPath).filePath("signatures");
        if (!QDir().mkpath(path)) {
            *error = "Could not create the local signature folder.";
            return {};
        }
        return path;
    }

    QString safeSignatureName(QString name)
    {
        name.replace(QRegularExpression(R"([\\/:*?"<>|])"), "_");
        name.remove(QRegularExpression(R"([\x00-\x1f])"));
        name = name.trimmed();
        while (name.endsWith('.') || name.endsWith(' '))
            name.chop(1);
        if (name.isEmpty())
            name = "Signature";
        if (name.size() > 80)
            name.truncate(80);
        return name;
    }

    QString uniqueSignatureFileName(const QDir &directory, const QString &name,
                                    const QString &exceptPath = QString())
    {
        QString fileName = name + ".png";
        int suffix = 1;
        while (directory.exists(fileName)
                   && (exceptPath.isEmpty()
                       || QFileInfo(directory.filePath(fileName)).absoluteFilePath()
                              != QFileInfo(exceptPath).absoluteFilePath())) {
            fileName = name + " (" + QString::number(suffix++) + ").png";
        }
        return fileName;
    }

    QImage trimSignatureImage(const QImage &image)
    {
        QRect bounds;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (qAlpha(image.pixel(x, y)) != 0)
                    bounds |= QRect(x, y, 1, 1);
            }
        }
        if (bounds.isEmpty())
            return {};
        return image.copy(bounds.adjusted(-20, -20, 20, 20)
                              .intersected(image.rect()));
    }

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

    QImage readImageFile(const QString &path, QString *error)
    {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        const QSize sourceSize = reader.size();
        if (sourceSize.isValid()
            && qMax(sourceSize.width(), sourceSize.height()) > kMaxImageSide) {
            QSize scaledSize = sourceSize;
            scaledSize.scale(kMaxImageSide, kMaxImageSide, Qt::KeepAspectRatio);
            reader.setScaledSize(scaledSize);
        }
        const QImage image = reader.read();
        if (image.isNull())
            *error = reader.errorString();
        return image;
    }

    QIcon signaturePreviewIcon(const QImage &image)
    {
        QImage preview(QSize(240, 82), QImage::Format_ARGB32_Premultiplied);
        preview.fill(Qt::white);
        QPainter painter(&preview);
        painter.setPen(QColor(190, 190, 190));
        painter.drawRect(preview.rect().adjusted(0, 0, -1, -1));

        const QSize availableSize = preview.size() - QSize(16, 16);
        const QImage scaled = image.scaled(
            availableSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        const QRect target(QPoint((preview.width() - scaled.width()) / 2,
                                  (preview.height() - scaled.height()) / 2),
                          scaled.size());
        painter.drawImage(target, scaled);
        return QIcon(QPixmap::fromImage(preview));
    }

    class SignatureCanvas : public QWidget
    {
    public:
        explicit SignatureCanvas(QWidget *parent = nullptr)
            : QWidget(parent)
        {
            setFixedSize(720, 216);
            setMouseTracking(true);
            setCursor(Qt::CrossCursor);
        }

        void setSignatureColor(const QColor &color)
        {
            m_color = color;
            update();
        }

        bool hasStrokes() const { return !m_strokes.isEmpty(); }

        void clear()
        {
            m_strokes.clear();
            update();
        }

        QImage signatureImage() const
        {
            QImage image(kSignatureImageSize, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.scale(static_cast<qreal>(image.width()) / width(),
                          static_cast<qreal>(image.height()) / height());
            drawStrokes(painter, 4.0, m_color);
            painter.end();
            return trimSignatureImage(image);
        }

    protected:
        void paintEvent(QPaintEvent *) override
        {
            QPainter painter(this);
            painter.fillRect(rect(), Qt::white);
            painter.setPen(QPen(palette().color(QPalette::Mid), 1));
            painter.drawRect(rect().adjusted(0, 0, -1, -1));
            painter.setRenderHint(QPainter::Antialiasing);
            drawStrokes(painter, 4.0, m_color);
        }

        void mousePressEvent(QMouseEvent *event) override
        {
            if (event->button() != Qt::LeftButton)
                return;
            m_strokes.append(QVector<QPointF>{event->position()});
            update();
        }

        void mouseMoveEvent(QMouseEvent *event) override
        {
            if (m_strokes.isEmpty() || !(event->buttons() & Qt::LeftButton))
                return;
            m_strokes.last().append(event->position());
            update();
        }

    private:
        void drawStrokes(QPainter &painter, qreal width, const QColor &color) const
        {
            painter.setPen(QPen(color, width, Qt::SolidLine,
                                Qt::RoundCap, Qt::RoundJoin));
            for (const QVector<QPointF> &stroke : m_strokes) {
                if (stroke.size() == 1) {
                    painter.drawPoint(stroke.first());
                } else {
                    for (qsizetype i = 1; i < stroke.size(); ++i)
                        painter.drawLine(stroke.at(i - 1), stroke.at(i));
                }
            }
        }

        QVector<QVector<QPointF>> m_strokes;
        QColor m_color = QColor(24, 55, 91);
    };

    class SignatureDrawDialog : public QDialog
    {
    public:
        explicit SignatureDrawDialog(QWidget *parent)
            : QDialog(parent)
        {
            setWindowTitle("Draw signature");
            auto *layout = new QVBoxLayout(this);
            layout->addWidget(new QLabel(
                "Draw your signature below. The saved mark has a transparent background.",
                this));

            auto *colorRow = new QHBoxLayout;
            colorRow->addWidget(new QLabel("Ink color:", this));
            auto *colorButton = new ColorMenuButton(QColor(24, 55, 91), this);
            colorRow->addWidget(colorButton);
            colorRow->addStretch();
            layout->addLayout(colorRow);

            m_canvas = new SignatureCanvas(this);
            layout->addWidget(m_canvas);
            connect(colorButton, &ColorMenuButton::colorChanged,
                    m_canvas, &SignatureCanvas::setSignatureColor);

            auto *buttons = new QHBoxLayout;
            auto *clearButton = new QPushButton("Clear", this);
            auto *cancelButton = new QPushButton("Cancel", this);
            auto *useButton = new QPushButton("Use signature", this);
            useButton->setDefault(true);
            buttons->addWidget(clearButton);
            buttons->addStretch();
            buttons->addWidget(cancelButton);
            buttons->addWidget(useButton);
            layout->addLayout(buttons);

            connect(clearButton, &QPushButton::clicked,
                    m_canvas, &SignatureCanvas::clear);
            connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
            connect(useButton, &QPushButton::clicked, this, [this]() {
                if (m_canvas->hasStrokes())
                    accept();
                else
                    QMessageBox::information(this, "Draw signature",
                                             "Draw a signature before using it.");
            });
        }

        QImage signatureImage() const { return m_canvas->signatureImage(); }

    private:
        SignatureCanvas *m_canvas;
    };

    class TypedSignatureDialog : public QDialog
    {
    public:
        explicit TypedSignatureDialog(QWidget *parent)
            : QDialog(parent)
        {
            setWindowTitle("Type signature");
            auto *layout = new QVBoxLayout(this);
            layout->addWidget(new QLabel("Enter your name and choose a font:", this));

            m_text = new QLineEdit(this);
            m_text->setPlaceholderText("Your name");
            layout->addWidget(m_text);

            m_font = new QFontComboBox(this);
            layout->addWidget(m_font);

            auto *colorRow = new QHBoxLayout;
            colorRow->addWidget(new QLabel("Signature color:", this));
            m_colorButton = new ColorMenuButton(QColor(24, 55, 91), this);
            colorRow->addWidget(m_colorButton);
            colorRow->addStretch();
            layout->addLayout(colorRow);

            m_preview = new QLabel(this);
            m_preview->setAlignment(Qt::AlignCenter);
            m_preview->setMinimumSize(420, 100);
            m_preview->setStyleSheet(
                "QLabel { color: #18375b; background: white; "
                "border: 1px solid palette(mid); }");
            layout->addWidget(m_preview);

            auto *buttons = new QHBoxLayout;
            auto *cancelButton = new QPushButton("Cancel", this);
            auto *useButton = new QPushButton("Use signature", this);
            useButton->setDefault(true);
            buttons->addStretch();
            buttons->addWidget(cancelButton);
            buttons->addWidget(useButton);
            layout->addLayout(buttons);

            QFont preferred = m_font->currentFont();
            const QStringList preferredFamilies = {
                "Segoe Script", "Lucida Handwriting", "Bradley Hand ITC",
                "French Script MT", "Edwardian Script ITC", "Segoe Print"
            };
            const QStringList installedFamilies = QFontDatabase::families();
            for (const QString &family : preferredFamilies) {
                const auto it = std::find_if(
                    installedFamilies.cbegin(), installedFamilies.cend(),
                    [&family](const QString &installed) {
                        return installed.compare(family, Qt::CaseInsensitive) == 0;
                    });
                if (it != installedFamilies.cend()) {
                    preferred.setFamily(*it);
                    break;
                }
            }
            m_font->setCurrentFont(preferred);
            connect(m_text, &QLineEdit::textChanged, this, [this]() { updatePreview(); });
            connect(m_font, &QFontComboBox::currentFontChanged,
                    this, [this]() { updatePreview(); });
            connect(m_colorButton, &ColorMenuButton::colorChanged,
                    this, [this]() { updatePreview(); });
            connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
            connect(useButton, &QPushButton::clicked, this, [this]() {
                if (!m_text->text().trimmed().isEmpty())
                    accept();
                else
                    QMessageBox::information(this, "Type signature",
                                             "Enter a name before using the signature.");
            });
            m_text->setFocus();
            updatePreview();
        }

        QString text() const { return m_text->text().trimmed(); }
        QString fontFamily() const { return m_font->currentFont().family(); }
        QColor color() const { return m_colorButton->color(); }

    private:
        void updatePreview()
        {
            QFont font = m_font->currentFont();
            font.setPointSize(30);
            m_preview->setFont(font);
            m_preview->setStyleSheet(
                QString("QLabel { color: %1; background: white; "
                        "border: 1px solid palette(mid); }")
                    .arg(m_colorButton->color().name()));
            m_preview->setText(m_text->text().isEmpty()
                                   ? "Signature preview"
                                   : m_text->text());
        }

        QLineEdit *m_text;
        QFontComboBox *m_font;
        QLabel *m_preview;
        ColorMenuButton *m_colorButton = nullptr;
    };

    QImage typedSignatureImage(const QString &text, const QString &family,
                               const QColor &color)
    {
        QImage image(kSignatureImageSize, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);

        QFont font(family);
        font.setStyleHint(QFont::Cursive);
        font.setPixelSize(170);
        const QFontMetricsF initialMetrics(font);
        if (initialMetrics.horizontalAdvance(text) > image.width() - 80) {
            const int fittedSize = qMax(
                24, static_cast<int>(
                        font.pixelSize() * (image.width() - 80)
                        / initialMetrics.horizontalAdvance(text)));
            font.setPixelSize(fittedSize);
        }

        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::TextAntialiasing);
        painter.setFont(font);
        painter.setPen(color);
        painter.drawText(image.rect().adjusted(30, 20, -30, -20),
                         Qt::AlignCenter, text);
        return trimSignatureImage(image);
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
    EditOperationGuard operation(this);

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

    if (refreshAfterEdit(viewPage, pages, sizes, true))
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

    EditOperationGuard operation(this);
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
    if (!m_editor || m_documentLoadPending || m_documentOperationPending)
        return;
    EditOperationGuard operation(this);

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

    EditOperationGuard operation(this);
    PdfEditor extractor;
    if (!extractor.open(m_history.current(), m_password)
        || !extractor.keepOnlyPages(dialog.firstPage(), dialog.lastPage())
        || !extractor.saveAs(dest, true, saveEncryption(), m_newPassword)) {
        QMessageBox::warning(this, "Extract failed", extractor.lastError());
        return;
    }

    statusBar()->showMessage("Extracted pages to " + QFileInfo(dest).fileName(), 4000);
}

void MainWindow::exportPages(const QString &format)
{
    m_view->commitPendingEdits();
    if (!m_editor || (format != QLatin1String("png") && format != QLatin1String("svg")))
        return;

    PageRangeDialog rangeDialog(m_document->pageCount(), m_view->currentPage(),
                                this, "Export pages");
    if (rangeDialog.exec() != QDialog::Accepted)
        return;

    const QString fileType = format.toUpper();
    const QString defaultName =
        QFileInfo(m_currentPath).completeBaseName() + QLatin1Char('.') + format;
    const QString selectedPath = QFileDialog::getSaveFileName(
        this, QString("Choose a filename for %1 export (multi-page exports are numbered)")
                    .arg(fileType),
        defaultName,
        format == QLatin1String("png")
            ? "PNG images (*.png)"
            : "SVG files (*.svg)");
    if (selectedPath.isEmpty())
        return;

    const QFileInfo selectedInfo(selectedPath);
    const QString baseName = selectedInfo.completeBaseName();
    const QString extension = QLatin1Char('.') + format;
    const int firstPage = rangeDialog.firstPage();
    const int lastPage = rangeDialog.lastPage();
    const int pageCount = lastPage - firstPage + 1;
    const QDir outputDirectory(selectedInfo.absolutePath());
    QStringList outputPaths;
    outputPaths.reserve(pageCount);
    for (int page = firstPage; page <= lastPage; ++page) {
        const QString fileName = pageCount == 1
            ? baseName + extension
            : QStringLiteral("%1-%2%3")
                  .arg(baseName)
                  .arg(page + 1, 3, 10, QLatin1Char('0'))
                  .arg(extension);
        outputPaths.append(outputDirectory.filePath(fileName));
    }

    QStringList existingFiles;
    for (const QString &path : std::as_const(outputPaths)) {
        if (QFileInfo::exists(path))
            existingFiles.append(QFileInfo(path).fileName());
    }
    if (!existingFiles.isEmpty()) {
        const QString fileList = existingFiles.size() <= 5
            ? existingFiles.join(QStringLiteral("\n"))
            : QStringLiteral("%1 and %2 other file(s)")
                  .arg(existingFiles.first())
                  .arg(existingFiles.size() - 1);
        if (QMessageBox::question(
                this, "Overwrite export files",
                "These files already exist:\n" + fileList
                    + "\n\nDo you want to overwrite them?",
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            != QMessageBox::Yes)
            return;
    }

    EditOperationGuard operation(this);
    if (!m_editor->exportPages(outputPaths, format, firstPage)) {
        const QString partialOutput = pageCount > 1
            ? "\nSome earlier pages may already have been exported."
            : QString();
        QMessageBox::warning(this, "Export failed",
                             m_editor->lastError() + partialOutput);
        return;
    }
    statusBar()->showMessage(
        QString("Exported %1 page(s) as %2.")
            .arg(pageCount)
            .arg(fileType),
        5000);
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

    EditOperationGuard operation(this);
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

    EditOperationGuard operation(this);
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
    EditOperationGuard operation(this);

    if (!m_editor->addTextBox(page, box, text, annotationFontName(style),
                              style.sizePoints, style.color.redF(),
                              style.color.greenF(), style.color.blueF())) {
        QMessageBox::warning(this, "Add text failed", m_editor->lastError());
        reopenEditor(m_history.current());
        updateActions();
        return;
    }
    if (refreshAfterPageEdit({ page }, page))
        m_view->preserveTextAnnotationChange(page, {}, box, text, style);
}

void MainWindow::updateTextAnnotation(int page, int annotationId,
                                      const QString &oldText, const QString &newText,
                                      const PageView::TextStyle &style)
{
    if (!m_editor)
        return;

    if (newText.trimmed().isEmpty()) {
        QMessageBox::warning(this, "Edit added text",
                             "Text cannot be empty. Use Delete added text to remove it.");
        m_view->editTextAnnotationInline(page, annotationId, oldText);
        return;
    }

    EditOperationGuard operation(this);
    if (!m_editor->updateTextAnnotation(page, annotationId, oldText, newText,
                                        annotationFontName(style), style.sizePoints,
                                        style.color.redF(), style.color.greenF(),
                                        style.color.blueF())) {
        QMessageBox::warning(this, "Edit added text", m_editor->lastError());
        reopenEditor(m_history.current());
        updateActions();
        return;
    }
    if (refreshAfterPageEdit({ page }, page))
        m_view->preserveTextAnnotationChange(
            page, m_view->selectedTextAnnotationBounds(),
            m_view->selectedTextAnnotationBounds(), newText, style);
}

void MainWindow::deleteTextAnnotation(int page, int annotationId, const QString &text)
{
    if (!m_editor)
        return;
    EditOperationGuard operation(this);

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
                                            const QString &text,
                                            const QRectF &oldBounds,
                                            const QRectF &bounds,
                                            const PageView::TextStyle &style)
{
    if (!m_editor)
        return;
    EditOperationGuard operation(this);

    if (!m_editor->setTextAnnotationBounds(page, annotationId, text, bounds)) {
        QMessageBox::warning(this, "Update text box failed", m_editor->lastError());
        reopenEditor(m_history.current());
        updateActions();
        return;
    }
    if (refreshAfterPageEdit({ page }, page))
        m_view->preserveTextAnnotationChange(page, oldBounds, bounds, text, style);
}

void MainWindow::updateImageStampBounds(int page, const QString &id,
                                        const QRectF &oldBounds,
                                        const QRectF &bounds)
{
    Q_UNUSED(oldBounds);
    if (!m_editor)
        return;
    EditOperationGuard operation(this);

    if (!m_editor->setImageStampBounds(page, id, bounds)) {
        QMessageBox::warning(this, "Move or resize image failed", m_editor->lastError());
        reopenEditor(m_history.current());
        updateActions();
        return;
    }
    if (refreshAfterPageEdit({ page }, page))
        statusBar()->showMessage("Updated added image.", 3000);
}

void MainWindow::deleteImageStamp(int page, const QString &id)
{
    if (!m_editor)
        return;
    EditOperationGuard operation(this);

    if (!m_editor->deleteImageStamp(page, id)) {
        QMessageBox::warning(this, "Delete added image", m_editor->lastError());
        reopenEditor(m_history.current());
        updateActions();
        return;
    }
    if (refreshAfterPageEdit({ page }, page))
        statusBar()->showMessage("Deleted added image.", 3000);
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

    QString error;
    const QImage image = readImageFile(path, &error);
    if (image.isNull()) {
        QMessageBox::warning(this, "Add image",
                             "Could not read that image: " + error);
        return;
    }

    m_addTextAction->setChecked(false);
    m_drawAction->setChecked(false);

    m_view->beginImagePlacement(image);
    statusBar()->showMessage(
        "Click on the page where the image goes. Drag to move or resize. Use Crop to crop, "
        "Enter or a click elsewhere places it, Cancel or Esc stops placement.", 12000);
}

void MainWindow::addTypedESignature()
{
    TypedSignatureDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QImage image = typedSignatureImage(
        dialog.text(), dialog.fontFamily(), dialog.color());
    saveESignature(image, dialog.text());
    beginESignaturePlacement(image);
}

void MainWindow::drawESignature()
{
    SignatureDrawDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QImage image = dialog.signatureImage();
    saveESignature(image, "Handwritten signature");
    beginESignaturePlacement(image);
}

void MainWindow::uploadESignature()
{
    const QString path = QFileDialog::getOpenFileName(
        this, "Choose a signature image", QString(),
        "Images (*.png *.jpg *.jpeg *.bmp *.gif *.webp *.tif *.tiff);;All files (*)");
    if (path.isEmpty())
        return;

    QString error;
    const QImage image = readImageFile(path, &error);
    if (image.isNull()) {
        QMessageBox::warning(this, "Upload signature",
                             "Could not read that image: " + error);
        return;
    }
    saveESignature(image, QFileInfo(path).completeBaseName());
    beginESignaturePlacement(image);
}

void MainWindow::saveESignature(const QImage &image, const QString &name)
{
    QString error;
    const QString directory = signatureStorageDirectory(&error);
    if (directory.isEmpty()) {
        QMessageBox::warning(this, "Save signature", error);
        return;
    }

    const QString fileBaseName = safeSignatureName(name);
    QDir signatureDirectory(directory);
    const QString fileName = uniqueSignatureFileName(signatureDirectory, fileBaseName);

    QSaveFile file(signatureDirectory.filePath(fileName));
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, "Save signature",
                             "Could not save this signature: " + file.errorString());
        return;
    }
    if (!image.save(&file, "PNG") || !file.commit()) {
        const QString saveError = file.errorString();
        file.cancelWriting();
        QMessageBox::warning(this, "Save signature",
                             "Could not save this signature: " + saveError);
        return;
    }
    statusBar()->showMessage(
        QString("Saved \"%1\" on this device for reuse.")
            .arg(QFileInfo(fileName).completeBaseName()),
        5000);
}

void MainWindow::manageESignatures()
{
    QString error;
    const QString directory = signatureStorageDirectory(&error);
    if (directory.isEmpty()) {
        QMessageBox::warning(this, "Saved signatures", error);
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle("Saved signatures");
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(
        "Saved signatures are stored locally on this device.", &dialog));
    auto *list = new QListWidget(&dialog);
    list->setIconSize(QSize(240, 82));
    list->setMinimumSize(460, 200);
    layout->addWidget(list);

    const auto refreshList = [list, directory](const QString &selectedPath = QString()) {
        list->clear();
        const QDir folder(directory);
        const QFileInfoList files = folder.entryInfoList(
            QStringList() << "*.png", QDir::Files, QDir::Name);
        for (const QFileInfo &info : files) {
            QString imageError;
            const QImage image = readImageFile(info.absoluteFilePath(), &imageError);
            auto *item = new QListWidgetItem(
                image.isNull() ? QIcon() : signaturePreviewIcon(image),
                info.completeBaseName(), list);
            item->setData(Qt::UserRole, info.absoluteFilePath());
            if (image.isNull())
                item->setToolTip("Preview unavailable: " + imageError);
            if (!selectedPath.isEmpty()
                && info.absoluteFilePath() == selectedPath)
                list->setCurrentItem(item);
        }
    };
    refreshList();
    dialog.resize(600, 400);

    auto *buttons = new QHBoxLayout;
    auto *useButton = new QPushButton("Use selected", &dialog);
    auto *renameButton = new QPushButton("Rename...", &dialog);
    auto *deleteButton = new QPushButton("Delete selected", &dialog);
    auto *closeButton = new QPushButton("Close", &dialog);
    useButton->setDefault(true);
    buttons->addWidget(useButton);
    buttons->addWidget(renameButton);
    buttons->addWidget(deleteButton);
    buttons->addStretch();
    buttons->addWidget(closeButton);
    layout->addLayout(buttons);

    connect(useButton, &QPushButton::clicked, &dialog, [&dialog, list]() {
        if (list->currentItem())
            dialog.accept();
    });
    connect(list, &QListWidget::itemDoubleClicked, &dialog,
            [&dialog](QListWidgetItem *) { dialog.accept(); });
    connect(renameButton, &QPushButton::clicked, &dialog,
            [this, list, directory, refreshList]() {
        QListWidgetItem *item = list->currentItem();
        if (!item)
            return;

        const QString oldPath = item->data(Qt::UserRole).toString();
        const QString oldName = item->text();
        bool accepted = false;
        const QString requestedName = QInputDialog::getText(
            this, "Rename saved signature", "Signature name:",
            QLineEdit::Normal, oldName, &accepted).trimmed();
        if (!accepted || requestedName.isEmpty())
            return;

        const QString newName = safeSignatureName(requestedName);
        const QDir folder(directory);
        const QString newFileName =
            uniqueSignatureFileName(folder, newName, oldPath);
        const QString newPath = folder.filePath(newFileName);
        if (QFileInfo(oldPath).absoluteFilePath()
            == QFileInfo(newPath).absoluteFilePath())
            return;

        QFile file(oldPath);
        if (!file.rename(newPath)) {
            QMessageBox::warning(this, "Rename saved signature",
                                 "Could not rename the signature: "
                                     + file.errorString());
            return;
        }
        refreshList(newPath);
    });
    connect(deleteButton, &QPushButton::clicked, &dialog,
            [this, list, refreshList]() {
        QListWidgetItem *item = list->currentItem();
        if (!item)
            return;
        const QString name = item->text();
        if (QMessageBox::question(
                this, "Delete saved signature",
                QString("Delete \"%1\" from this device?").arg(name),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            != QMessageBox::Yes)
            return;

        QFile file(item->data(Qt::UserRole).toString());
        if (!file.remove()) {
            QMessageBox::warning(this, "Delete saved signature",
                                 "Could not delete the signature: " + file.errorString());
            return;
        }
        refreshList();
    });
    connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted || !list->currentItem())
        return;

    QString imageError;
    const QString filePath = list->currentItem()->data(Qt::UserRole).toString();
    const QImage image = readImageFile(filePath, &imageError);
    if (image.isNull()) {
        QMessageBox::warning(this, "Use saved signature",
                             "Could not read that signature: " + imageError);
        return;
    }
    beginESignaturePlacement(image);
}

void MainWindow::beginESignaturePlacement(const QImage &image)
{
    if (!m_editor || m_documentLoadPending || m_documentOperationPending
        || m_editor->hasDigitalSignatures())
        return;
    if (image.isNull()) {
        QMessageBox::warning(this, "E-sign", "Could not create the signature image.");
        return;
    }

    m_view->commitPendingEdits();
    m_addTextAction->setChecked(false);
    m_drawAction->setChecked(false);
    m_redactAction->setChecked(false);
    m_signAction->setChecked(false);
    m_view->beginImagePlacement(image);
    statusBar()->showMessage(
        "Click on the page to place your E-sign mark. Drag to move or resize it; "
        "press Enter or click elsewhere to commit, or press Esc to cancel. "
        "This is a visible mark, not a certificate-based digital signature.",
        14000);
}

void MainWindow::placeImage(int page, const QRectF &box, const QImage &image)
{
    if (!m_editor)
        return;
    EditOperationGuard operation(this);

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

    EditOperationGuard operation(this);
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
    EditOperationGuard operation(this);
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
    EditOperationGuard operation(this);

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
    if (refreshAfterPageEdit(changedPages, strokes.last().page)) {
        statusBar()->showMessage(QString("Added %1 stroke(s)").arg(strokes.size()), 3000);
    }
}

void MainWindow::eraseInk(int page, const QVector<QPointF> &path, double radius)
{
    if (!m_editor)
        return;
    EditOperationGuard operation(this);

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

    if (refreshAfterPageEdit({ page }, page)) {
        statusBar()->showMessage(
            QString("Erased %1 drawing stroke(s)").arg(removed), 3000);
    }
}

void MainWindow::undo()
{
    if (m_documentLoadPending || m_documentOperationPending)
        return;
    if (m_view->undoLastStroke())
        return;
    stepHistory(m_history.undo());
}

void MainWindow::redo()
{
    if (m_documentLoadPending || m_documentOperationPending)
        return;
    stepHistory(m_history.redo());
}

void MainWindow::stepHistory(const QString &path)
{
    if (path.isEmpty())
        return;
    EditOperationGuard operation(this);

    const int page = m_view->currentPage();
    if (!showFile(path, page, true, false, true))
        return;

    refreshFormFieldOverlays();

    updateActions();
    updateTitle();
}