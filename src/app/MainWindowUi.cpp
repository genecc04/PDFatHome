#include "MainWindow.h"
#include "BookmarkPane.h"
#include "ColorMenuButton.h"
#include "PdfEditor.h"
#include "SearchPane.h"
#include "ThumbnailPane.h"

#include <QAction>
#include <QActionGroup>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QDockWidget>
#include <QDesktopServices>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>

#include <utility>

namespace
{
constexpr int kToolbarIconSize = 32;
constexpr int kNumericControlWidth = 88;
}

void MainWindow::setupViewer()
{
    m_view = new PageView(this);
    m_view->setDocument(m_document);
    m_view->setZoomMode(PageView::ZoomMode::FitToWidth);
    setCentralWidget(m_view);

    connect(m_view, &PageView::textPlaced, this, &MainWindow::placeText);
    connect(m_view, &PageView::textAnnotationEditRequested, m_view,
            &PageView::editTextAnnotationInline);
    connect(m_view, &PageView::textAnnotationEdited,
            this, &MainWindow::updateTextAnnotation);
    connect(m_view, &PageView::textAnnotationDeleteRequested,
            this, &MainWindow::deleteTextAnnotation);
    connect(m_view, &PageView::textAnnotationBoundsChanged,
            this, &MainWindow::updateTextAnnotationBounds);
    connect(m_view, &PageView::textAnnotationSelectionChanged, this, [this](bool selected) {
        if (m_deleteTextAction)
            m_deleteTextAction->setEnabled(
                selected && m_editor != nullptr && !m_documentLoadPending);
    });
    connect(m_view, &PageView::imageStampBoundsChanged,
            this, &MainWindow::updateImageStampBounds);
    connect(m_view, &PageView::imageStampDeleteRequested,
            this, &MainWindow::deleteImageStamp);
    connect(m_view, &PageView::imageStampSelectionChanged, this, [this](bool selected) {
        if (m_deleteImageAction)
            m_deleteImageAction->setEnabled(
                selected && m_editor != nullptr && !m_documentLoadPending);
    });
    connect(m_view, &PageView::textToolFinished, this, [this]() {
        m_addTextAction->setChecked(false);
    });
    connect(m_view, &PageView::strokesPlaced, this, &MainWindow::placeStrokes);
    connect(m_view, &PageView::eraseRequested, this, &MainWindow::eraseInk);
    connect(m_view, &PageView::pendingStrokesChanged, this, [this]() {
        updateActions();
        updateTitle();
    });
    connect(m_view, &PageView::pendingChangesChanged, this, [this]() {
        updateActions();
        updateTitle();
    });
    connect(m_view, &PageView::redactionAreasChanged, this, [this](int count) {
        updateActions();
        if (count > 0) {
            statusBar()->showMessage(
                QString("%1 redaction area(s) marked. Review them, then select Apply "
                        "Redactions to create a copy.")
                    .arg(count),
                8000);
        }
    });
    connect(m_view, &PageView::redactionToolChanged, this, [this](bool active) {
        QSignalBlocker blocker(m_redactAction);
        m_redactAction->setChecked(active);
    });
    connect(m_view, &PageView::signaturePlacementRequested,
            this, &MainWindow::signDocument);
    connect(m_view, &PageView::signatureToolChanged, this, [this](bool active) {
        QSignalBlocker blocker(m_signAction);
        m_signAction->setChecked(active);
    });
    connect(m_view, &PageView::imagePlaced, this, &MainWindow::placeImage);
    connect(m_view, &PageView::imagePlacementStateChanged, this,
            [this](bool active, bool positioned) {
        m_cancelImageAction->setEnabled(active && !m_documentLoadPending);
        m_cropImageAction->setEnabled(positioned && !m_documentLoadPending);
        if (!positioned) {
            QSignalBlocker blocker(m_cropImageAction);
            m_cropImageAction->setChecked(false);
        }
    });
    connect(m_view, &PageView::imageCropModeChanged, this, [this](bool active) {
        {
            QSignalBlocker blocker(m_cropImageAction);
            m_cropImageAction->setChecked(active);
        }
        if (active) {
            statusBar()->showMessage(
                "Crop mode: drag a corner inward to crop. Select Crop again to finish cropping.",
                12000);
        } else if (m_cancelImageAction->isEnabled()) {
            statusBar()->showMessage(
                "Image placement: drag to move or resize. Select Crop to crop; Enter places; "
                "Esc cancels.",
                12000);
        } else {
            statusBar()->clearMessage();
        }
    });
    m_view->setFormFieldHandler(
        [this](int page, const QPointF &point, PageView::FormField *field) {
            return fillFormField(page, point, field);
        });
    m_view->setFormFieldValueHandler(
        [this](int page, int widgetIndex, const QString &value) {
            return commitFormField(page, widgetIndex, value);
        });
    m_view->setTextAnnotationHandler(
        [this](int page, const QPointF &point, int *annotationId,
               QRectF *bounds, QString *text, PageView::TextStyle *style) {
            QString fontName;
            double fontSize = 12.0;
            QColor color;
            if (!m_editor
                || !m_editor->textAnnotationAt(page, point, annotationId, bounds, text,
                                               &fontName, &fontSize, &color))
                return false;

            style->family = fontName == QLatin1String("TiRo")
                ? QStringLiteral("Times")
                : fontName == QLatin1String("Cour")
                    ? QStringLiteral("Courier")
                    : QStringLiteral("Helvetica");
            style->sizePoints = fontSize;
            style->color = color;
            return true;
        });
    m_view->setImageStampHandler(
        [this](int page, const QPointF &point, PdfEditor::ImageStamp *stamp) {
            if (!m_editor)
                return false;
            const bool found = m_editor->imageStampAt(page, point, stamp);
            if (!found && !m_editor->lastError().isEmpty())
                QMessageBox::warning(this, "Select added image", m_editor->lastError());
            return found;
        });
    m_view->setInkStrokeHandler(
        [this](int page, QVector<PageView::Stroke> *strokes) {
            if (!m_editor)
                return false;

            QVector<PdfEditor::InkStroke> inkStrokes;
            if (!m_editor->inkStrokesOnPage(page, &inkStrokes)) {
                QMessageBox::warning(this, "Erase failed", m_editor->lastError());
                return false;
            }

            strokes->clear();
            strokes->reserve(inkStrokes.size());
            for (PdfEditor::InkStroke &inkStroke : inkStrokes) {
                PageView::Stroke stroke;
                stroke.page = page;
                stroke.points = std::move(inkStroke.points);
                stroke.style.widthPoints = inkStroke.widthPoints;
                strokes->append(std::move(stroke));
            }
            return true;
        });
    connect(m_view, &PageView::openRequested, this, &MainWindow::openFile);
    connect(m_view, &PageView::linkHovered, this, [this](const QUrl &url) {
        if (url.isEmpty())
            statusBar()->clearMessage();
        else
            statusBar()->showMessage(url.toDisplayString());
    });
    connect(m_view, &PageView::externalLinkRequested, this, [this](const QUrl &url) {
        const QString scheme = url.scheme().toLower();
        if (!url.isValid() || url.host().isEmpty()
            || (scheme != QLatin1String("http") && scheme != QLatin1String("https"))) {
            QMessageBox::warning(
                this, "Link not opened",
                "Only valid HTTP and HTTPS website links can be opened.");
            return;
        }

        QMessageBox confirmation(QMessageBox::Question, "Open website",
                                 "Do you want to go to this site?\n\n"
                                     + url.toDisplayString(QUrl::RemovePassword),
                                 QMessageBox::NoButton, this);
        confirmation.setTextFormat(Qt::PlainText);
        QPushButton *proceed = confirmation.addButton(
            "Proceed", QMessageBox::AcceptRole);
        QPushButton *cancel = confirmation.addButton(
            QMessageBox::Cancel);
        confirmation.setDefaultButton(cancel);
        confirmation.setEscapeButton(cancel);
        confirmation.exec();
        if (confirmation.clickedButton() != proceed)
            return;

        if (!QDesktopServices::openUrl(url)) {
            QMessageBox::warning(
                this, "Could not open website",
                "Windows could not open the link in your default browser.");
        }
    });
}

void MainWindow::setupThumbnailPane()
{
    m_thumbnails = new ThumbnailPane(this);
    m_thumbnails->setDocument(m_document);

    m_pagesDock = new QDockWidget("Pages", this);
    m_pagesDock->setObjectName("pagesDock");
    m_pagesDock->setWidget(m_thumbnails);
    m_pagesDock->setFeatures(QDockWidget::DockWidgetClosable |
                             QDockWidget::DockWidgetMovable);
    addDockWidget(Qt::LeftDockWidgetArea, m_pagesDock);

    connect(m_thumbnails, &ThumbnailPane::pageRequested, this, &MainWindow::goToPage);
    connect(m_view, &PageView::currentPageChanged,
            m_thumbnails, &ThumbnailPane::setCurrentPage);
    connect(m_view, &PageView::visiblePageRangeChanged, this,
            [this](int firstPage, int lastPage) {
                refreshFormFieldOverlays(firstPage, lastPage);
            });

    connect(m_thumbnails, &ThumbnailPane::pageMoveRequested, this,
            &MainWindow::movePage, Qt::QueuedConnection);
}

void MainWindow::setupSearchPane()
{
    m_search = new SearchPane(this);
    m_search->setDocument(m_document, m_password);
    m_view->setSearchModel(m_search->searchModel());

    m_searchDock = new QDockWidget("Search", this);
    m_searchDock->setObjectName("searchDock");
    m_searchDock->setWidget(m_search);
    m_searchDock->setFeatures(QDockWidget::DockWidgetClosable |
                              QDockWidget::DockWidgetMovable);
    addDockWidget(Qt::LeftDockWidgetArea, m_searchDock);
    tabifyDockWidget(m_pagesDock, m_searchDock);
    m_pagesDock->raise();

    connect(m_search, &SearchPane::resultSelected, this,
            [this](int index, int page, QPointF location) {
                m_view->jumpToPage(page, location);
                m_view->setCurrentSearchResultIndex(index);
            });
}

void MainWindow::setupBookmarkPane()
{
    m_bookmarks = new BookmarkPane(this);
    m_bookmarks->setDocument(m_document);

    m_bookmarksDock = new QDockWidget("Bookmarks", this);
    m_bookmarksDock->setObjectName("bookmarksDock");
    m_bookmarksDock->setWidget(m_bookmarks);
    m_bookmarksDock->setFeatures(QDockWidget::DockWidgetClosable |
                                 QDockWidget::DockWidgetMovable);
    addDockWidget(Qt::LeftDockWidgetArea, m_bookmarksDock);
    tabifyDockWidget(m_pagesDock, m_bookmarksDock);
    m_pagesDock->raise();

    connect(m_bookmarks, &BookmarkPane::bookmarkSelected, this,
            [this](int page, QPointF location) {
                m_view->jumpToPage(page, location);
            });
}

void MainWindow::setupMenus()
{
    QMenu *fileMenu = menuBar()->addMenu("&File");
    QAction *openAction = fileMenu->addAction("&Open...");
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::openFile);

    m_recentMenu = fileMenu->addMenu("Open &Recent");
    updateRecentMenu();

    fileMenu->addSeparator();

    m_saveAction = fileMenu->addAction("&Save");
    m_saveAction->setShortcut(QKeySequence::Save);
    connect(m_saveAction, &QAction::triggered, this, &MainWindow::save);

    m_saveAsAction = fileMenu->addAction("Save &As...");
    m_saveAsAction->setShortcut(QKeySequence::SaveAs);
    connect(m_saveAsAction, &QAction::triggered, this, &MainWindow::saveAs);

    m_optimizeAction = fileMenu->addAction("Optimize PDF (lossless)...");
    connect(m_optimizeAction, &QAction::triggered, this, &MainWindow::optimizePdf);

    m_extractAction = fileMenu->addAction("&Extract pages...");
    connect(m_extractAction, &QAction::triggered, this, &MainWindow::extractPages);

    QMenu *exportMenu = fileMenu->addMenu("Export pages");
    m_exportPngAction = exportMenu->addAction("Export as &PNG images...");
    connect(m_exportPngAction, &QAction::triggered, this,
            [this]() { exportPages(QStringLiteral("png")); });
    m_exportSvgAction = exportMenu->addAction("Export as &SVG files...");
    connect(m_exportSvgAction, &QAction::triggered, this,
            [this]() { exportPages(QStringLiteral("svg")); });

    m_insertAction = fileMenu->addAction("&Insert pages from PDF...");
    connect(m_insertAction, &QAction::triggered, this, &MainWindow::insertPages);

    fileMenu->addSeparator();

    m_setPasswordAction = fileMenu->addAction("Set &password...");
    connect(m_setPasswordAction, &QAction::triggered, this, &MainWindow::setDocumentPassword);

    m_removePasswordAction = fileMenu->addAction("Re&move password");
    connect(m_removePasswordAction, &QAction::triggered, this, &MainWindow::removeDocumentPassword);

    QMenu *editMenu = menuBar()->addMenu("&Edit");
    m_undoAction = editMenu->addAction("&Undo");
    m_undoAction->setShortcut(QKeySequence::Undo);
    connect(m_undoAction, &QAction::triggered, this, &MainWindow::undo);

    m_redoAction = editMenu->addAction("&Redo");
    m_redoAction->setShortcut(QKeySequence::Redo);
    connect(m_redoAction, &QAction::triggered, this, &MainWindow::redo);

    editMenu->addSeparator();

    m_highlightAction = editMenu->addAction("&Highlight text...");
    m_highlightAction->setShortcut(Qt::CTRL | Qt::Key_H);
    connect(m_highlightAction, &QAction::triggered, this, &MainWindow::highlightText);

    m_addTextAction = editMenu->addAction("Add &text");
    m_addTextAction->setCheckable(true);
    m_addTextAction->setShortcut(Qt::CTRL | Qt::Key_T);
        connect(m_addTextAction, &QAction::toggled, this, [this](bool on) {
        if (on)
            m_view->commitPendingEdits();
        m_view->setTextToolActive(on);
        if (on) {
            statusBar()->showMessage(
                "Drag a box on the page and type. Click elsewhere to place it. "
                "Double-click added text to edit; right-click it to edit or delete.",
                10000);
        }
    });

    m_drawAction = editMenu->addAction("&Draw");
    m_drawAction->setCheckable(true);
    m_drawAction->setShortcut(Qt::CTRL | Qt::Key_D);
    connect(m_drawAction, &QAction::toggled, this, [this](bool on) {
        if (on)
            m_view->commitPendingEdits();
        m_view->setDrawToolActive(on);
        if (on) {
            statusBar()->showMessage(
                "Draw strokes or erase existing Ink strokes on contact. The eraser does not affect "
                "highlights or images. Ctrl+Z removes your last pending stroke. "
                "Turn Draw off to add everything to the document.", 10000);
        }
    });

    m_redactAction = editMenu->addAction("&Mark redaction");
    m_redactAction->setCheckable(true);
    m_redactAction->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_R);
    connect(m_redactAction, &QAction::toggled, this, [this](bool on) {
        if (on)
            m_view->commitPendingEdits();
        m_view->setRedactionToolActive(on);
        if (on) {
            statusBar()->showMessage(
                "Drag rectangles over content to redact. Review the marked areas, then select "
                "Apply Redactions to create a copy. The original PDF is not changed.",
                12000);
        } else if (m_view->hasRedactionAreas()) {
            statusBar()->showMessage(
                QString("%1 redaction area(s) marked. Apply them to create a redacted copy, "
                        "or clear the marks.")
                    .arg(m_view->redactionAreas().size()),
                10000);
        }
    });

    m_applyRedactionsAction = editMenu->addAction("Apply &redaction...");
    connect(m_applyRedactionsAction, &QAction::triggered,
            this, &MainWindow::applyRedactions);

    m_clearRedactionsAction = editMenu->addAction("Clear redaction areas");
    connect(m_clearRedactionsAction, &QAction::triggered,
            m_view, &PageView::clearRedactionAreas);

    m_signAction = editMenu->addAction("Certificate-based digital &signature...");
    m_signAction->setCheckable(true);
    connect(m_signAction, &QAction::toggled, this, [this](bool on) {
        if (on)
            m_view->commitPendingEdits();
        m_view->setSignatureToolActive(on);
        if (on) {
            statusBar()->showMessage(
                "Drag a rectangle where the visible digital signature should appear.",
                10000);
        }
    });

    auto *toolGroup = new QActionGroup(this);
    toolGroup->setExclusionPolicy(QActionGroup::ExclusionPolicy::ExclusiveOptional);
    toolGroup->addAction(m_addTextAction);
    toolGroup->addAction(m_drawAction);
    toolGroup->addAction(m_redactAction);
    toolGroup->addAction(m_signAction);

    m_addImageAction = editMenu->addAction("Add &image...");
    m_addImageAction->setShortcut(Qt::CTRL | Qt::Key_I);
    connect(m_addImageAction, &QAction::triggered, this, &MainWindow::addImage);

    QMenu *viewMenu = menuBar()->addMenu("&View");
    QAction *togglePages = m_pagesDock->toggleViewAction();
    togglePages->setShortcut(Qt::Key_F4);
    viewMenu->addAction(togglePages);
    QAction *toggleBookmarks = m_bookmarksDock->toggleViewAction();
    toggleBookmarks->setShortcut(Qt::CTRL | Qt::Key_B);
    viewMenu->addAction(toggleBookmarks);

    QAction *findAction = viewMenu->addAction("&Find...");
    findAction->setShortcut(QKeySequence::Find);
    connect(findAction, &QAction::triggered, this, [this]() {
        m_searchDock->show();
        m_searchDock->raise();
        m_search->focusSearchBox();
    });

    m_verifySignaturesAction =
        viewMenu->addAction("Verify digital &signatures...");
    connect(m_verifySignaturesAction, &QAction::triggered,
            this, &MainWindow::showDigitalSignatures);

    QMenu *helpMenu = menuBar()->addMenu("&Help");
    connect(helpMenu->addAction("&About"), &QAction::triggered, this, &MainWindow::showAbout);
    connect(helpMenu->addAction("About &Qt"), &QAction::triggered, qApp, &QApplication::aboutQt);
}

void MainWindow::setupToolbar()
{
    QToolBar *toolbar = addToolBar("Navigation");
    toolbar->setObjectName("navigationToolbar");
    toolbar->setMovable(false);
    toolbar->setIconSize(QSize(kToolbarIconSize, kToolbarIconSize));
    toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);

    // Page navigation
    QAction *prevAction = toolbar->addAction("< Prev");
    QAction *nextAction = toolbar->addAction("Next >");
    prevAction->setIcon(QIcon(":/icons/previous.png"));
    prevAction->setToolTip("Previous page");
    nextAction->setIcon(QIcon(":/icons/next.png"));
    nextAction->setToolTip("Next page");

    m_pageBox = new QSpinBox(this);
    m_pageBox->setRange(0, 0);
    toolbar->addWidget(m_pageBox);
    m_pageCountLabel = new QLabel(" / 0 ", this);
    toolbar->addWidget(m_pageCountLabel);

    toolbar->addSeparator();

    // Zoom
    QAction *zoomOutAction = toolbar->addAction("Zoom -");
    QAction *zoomInAction = toolbar->addAction("Zoom +");
    QAction *fitWidthAction = toolbar->addAction("Fit width");
    QAction *fitPageAction = toolbar->addAction("Fit page");
    zoomOutAction->setIcon(QIcon(":/icons/zoomout.png"));
    zoomInAction->setIcon(QIcon(":/icons/zoomin.png"));
    fitWidthAction->setIcon(QIcon(":/icons/fitwidth.png"));
    fitPageAction->setIcon(QIcon(":/icons/fitpage.png"));

    toolbar->addSeparator();

    // Editing
    m_rotateLeftAction = toolbar->addAction("Rotate left");
    m_rotateLeftAction->setIcon(QIcon(":/icons/rotateleft.png"));
    m_rotateLeftAction->setShortcut(Qt::CTRL | Qt::Key_L);
    m_rotateRightAction = toolbar->addAction("Rotate right");
    m_rotateRightAction->setIcon(QIcon(":/icons/rotateright.png"));
    m_rotateRightAction->setShortcut(Qt::CTRL | Qt::Key_R);
    connect(m_rotateLeftAction, &QAction::triggered, this, [this]() { rotateSelectedPages(-90); });
    connect(m_rotateRightAction, &QAction::triggered, this, [this]() { rotateSelectedPages(90); });

    m_deletePageAction = toolbar->addAction("Delete pages");
    m_deletePageAction->setIcon(QIcon(":/icons/deletepage.png"));
    connect(m_deletePageAction, &QAction::triggered, this, &MainWindow::deleteSelectedPages);

    // Wiring
    connect(prevAction, &QAction::triggered, this, [this]() {
        goToPage(m_view->currentPage() - 1);
    });
    connect(nextAction, &QAction::triggered, this, [this]() {
        goToPage(m_view->currentPage() + 1);
    });
    connect(m_pageBox, &QSpinBox::valueChanged, this, [this](int value) {
        goToPage(value - 1);
    });
    connect(m_view, &PageView::currentPageChanged, this, [this](int page) {
        QSignalBlocker blocker(m_pageBox);
        m_pageBox->setValue(page + 1);
    });
    connect(zoomInAction, &QAction::triggered, this, [this]() {
        m_view->setZoomFactor(m_view->zoomFactor() * 1.25);
    });
    connect(zoomOutAction, &QAction::triggered, this, [this]() {
        m_view->setZoomFactor(m_view->zoomFactor() / 1.25);
    });
    connect(fitWidthAction, &QAction::triggered, this, [this]() {
        m_view->setZoomMode(PageView::ZoomMode::FitToWidth);
    });
    connect(fitPageAction, &QAction::triggered, this, [this]() {
        m_view->setZoomMode(PageView::ZoomMode::FitInView);
    });
}

void MainWindow::setupImageToolbar()
{
    QToolBar *toolbar = addToolBar("Image");
    toolbar->setObjectName("imageToolbar");
    toolbar->setMovable(false);
    toolbar->setIconSize(QSize(kToolbarIconSize, kToolbarIconSize));
    toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);

    toolbar->addAction(m_addImageAction);
    m_addImageAction->setIcon(QIcon(":/icons/addimage.png"));
    toolbar->addSeparator();

    m_cropImageAction = toolbar->addAction("Crop");
    m_cropImageAction->setIcon(QIcon(":/icons/crop.png"));
    m_cropImageAction->setCheckable(true);
    m_cropImageAction->setEnabled(false);
    connect(m_cropImageAction, &QAction::toggled,
            m_view, &PageView::setImageCropMode);

    m_deleteImageAction = toolbar->addAction("Delete");
    m_deleteImageAction->setIcon(QIcon(":/icons/deleteimage.png"));
    m_deleteImageAction->setEnabled(false);
    m_deleteImageAction->setToolTip("Delete selected added image");
    connect(m_deleteImageAction, &QAction::triggered,
            m_view, &PageView::deleteSelectedImageStamp);

    m_cancelImageAction = toolbar->addAction("Cancel");
    m_cancelImageAction->setIcon(QIcon(":/icons/cancelimage.png"));
    m_cancelImageAction->setEnabled(false);
    m_cancelImageAction->setShortcut(QKeySequence(Qt::Key_Escape));
    m_cancelImageAction->setShortcutContext(Qt::WindowShortcut);
    addAction(m_cancelImageAction);
    connect(m_cancelImageAction, &QAction::triggered,
            m_view, &PageView::cancelImagePlacement);
}

void MainWindow::setupTextToolbar()
{
    QToolBar *toolbar = addToolBar("Text");
    toolbar->setObjectName("textToolbar");
    toolbar->setMovable(false);
    toolbar->setIconSize(QSize(kToolbarIconSize, kToolbarIconSize));
    toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    toolbar->setStyleSheet(
        "QToolButton:checked { border: 2px solid palette(highlight); border-radius: 4px; }");

    toolbar->addAction(m_addTextAction);
    m_addTextAction->setIcon(QIcon(":/icons/addtext.png"));
    m_addTextAction->setToolTip("Add text to the PDF (active when checked)");
    m_deleteTextAction = toolbar->addAction("Delete");
    m_deleteTextAction->setIcon(QIcon(":/icons/deletetext.png"));
    m_deleteTextAction->setEnabled(false);
    m_deleteTextAction->setToolTip("Delete selected text box");
    connect(m_deleteTextAction, &QAction::triggered,
            m_view, &PageView::deleteSelectedTextAnnotation);
    toolbar->addSeparator();

    auto *fontBox = new QComboBox(this);
    fontBox->addItems({ "Helvetica", "Times", "Courier" });
    fontBox->setCurrentText("Helvetica");
    fontBox->setMinimumWidth(130);
    fontBox->setFocusPolicy(Qt::NoFocus);
    fontBox->setProperty("textEditorStyleControl", true);
    toolbar->addWidget(fontBox);

    auto *size = new QSpinBox(this);
    size->setRange(6, 200);
    size->setValue(12);
    size->setSuffix(" pt");
    size->setMinimumWidth(kNumericControlWidth);
    size->setCorrectionMode(QAbstractSpinBox::CorrectToNearestValue);
    size->setKeyboardTracking(false);
    size->setProperty("textEditorStyleControl", true);
    toolbar->addWidget(size);

    auto *colorButton = new ColorMenuButton(QColor(0, 0, 0), this);
    colorButton->setProperty("textEditorStyleControl", true);
    toolbar->addWidget(colorButton);
    connect(m_view, &PageView::textAnnotationEditingStyleChanged, this,
            [fontBox, size, colorButton](const PageView::TextStyle &style) {
        const QSignalBlocker fontBlocker(fontBox);
        const QSignalBlocker sizeBlocker(size);
        const QSignalBlocker colorBlocker(colorButton);
        fontBox->setCurrentText(style.family);
        size->setValue(qRound(style.sizePoints));
        colorButton->setColor(style.color);
    });

    auto applyStyle = [this, fontBox, size, colorButton]() {
        PageView::TextStyle style;
        style.family = fontBox->currentText();
        style.sizePoints = size->value();
        style.color = colorButton->color();
        m_view->setTextStyle(style);
    };
    connect(fontBox, &QComboBox::currentTextChanged, this, applyStyle);
    connect(size, &QSpinBox::valueChanged, this, applyStyle);
    connect(colorButton, &ColorMenuButton::colorChanged, this, applyStyle);

    toolbar->addSeparator();
    applyStyle();
}

void MainWindow::setupDrawToolbar()
{
    QToolBar *toolbar = addToolBar("Draw");
    toolbar->setObjectName("drawToolbar");
    toolbar->setMovable(false);
    toolbar->setIconSize(QSize(kToolbarIconSize, kToolbarIconSize));
    toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    toolbar->setStyleSheet(
        "QToolButton:checked { border: 2px solid palette(highlight); border-radius: 4px; }");

    toolbar->addAction(m_drawAction);
    m_drawAction->setIcon(QIcon(":/icons/draw.png"));
    m_drawAction->setToolTip("Draw on the PDF (active when checked)");

    auto *mode = new QComboBox(this);
    mode->addItem("Pen");
    mode->addItem("Highlighter");
    mode->addItem("White-out");
    mode->addItem("Eraser");
    mode->setFocusPolicy(Qt::NoFocus);
    toolbar->addWidget(mode);

    toolbar->addWidget(new QLabel(" Size ", this));
    auto *size = new QSpinBox(this);
    size->setRange(1, 60);
    size->setValue(2);
    size->setSuffix(" pt");
    size->setMinimumWidth(kNumericControlWidth);
    size->setCorrectionMode(QAbstractSpinBox::CorrectToNearestValue);
    size->setKeyboardTracking(false);
    toolbar->addWidget(size);

    toolbar->addWidget(new QLabel(" Opacity ", this));
    auto *opacity = new QSpinBox(this);
    opacity->setRange(5, 100);
    opacity->setValue(100);
    opacity->setSuffix(" %");
    opacity->setMinimumWidth(kNumericControlWidth);
    opacity->setCorrectionMode(QAbstractSpinBox::CorrectToNearestValue);
    opacity->setKeyboardTracking(false);
    toolbar->addWidget(opacity);

    auto *colorButton = new ColorMenuButton(QColor(0, 0, 0), this);
    toolbar->addWidget(colorButton);

    auto applyStyle = [this, size, opacity, colorButton]() {
        PageView::DrawStyle style;
        style.color = colorButton->color();
        style.widthPoints = size->value();
        style.opacity = opacity->value() / 100.0;
        m_view->setDrawStyle(style);
    };
    connect(size, &QSpinBox::valueChanged, this, applyStyle);
    connect(opacity, &QSpinBox::valueChanged, this, applyStyle);
    connect(colorButton, &ColorMenuButton::colorChanged, this, applyStyle);

    connect(mode, &QComboBox::currentIndexChanged, this,
            [this, size, opacity, colorButton, applyStyle](int index) {
                if (index == 3)
                    m_view->commitPendingEdits();
                if (index == 0) {
                    size->setValue(2);
                    opacity->setValue(100);
                    colorButton->setColor(QColor(0, 0, 0));
                } else if (index == 1) {
                    size->setValue(14);
                    opacity->setValue(35);
                    colorButton->setColor(QColor(255, 220, 0));
                } else if (index == 2) {
                    size->setValue(16);
                    opacity->setValue(100);
                    colorButton->setColor(QColor(255, 255, 255));
                } else {
                    size->setValue(16);
                }
                opacity->setEnabled(index != 3);
                colorButton->setEnabled(index != 3);
                m_view->setEraserActive(index == 3);
                applyStyle();
            });
    applyStyle();
}

void MainWindow::setupSignatureToolbar()
{
    QToolBar *toolbar = addToolBar("Markup");
    toolbar->setObjectName("markupToolbar");
    toolbar->setMovable(false);
    toolbar->setIconSize(QSize(kToolbarIconSize, kToolbarIconSize));
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    toolbar->addAction(m_redactAction);
    m_redactAction->setIcon(QIcon(":/icons/markredaction.png"));
    m_redactAction->setText("Mark redaction");
    m_redactAction->setToolTip(
        "Mark rectangular areas for removal from a separate redacted PDF copy");
    if (QToolButton *button = qobject_cast<QToolButton *>(
            toolbar->widgetForAction(m_redactAction))) {
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    }

    toolbar->addAction(m_applyRedactionsAction);
    m_applyRedactionsAction->setIcon(QIcon(":/icons/applyredaction.png"));
    m_applyRedactionsAction->setText("Apply redaction");
    m_applyRedactionsAction->setToolTip(
        "Apply marked redactions and save a separate PDF copy");
    if (QToolButton *button = qobject_cast<QToolButton *>(
            toolbar->widgetForAction(m_applyRedactionsAction))) {
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    }

    toolbar->addSeparator();

    m_esignButton = new QToolButton(toolbar);
    m_esignButton->setText("E-sign");
    m_esignButton->setIcon(QIcon(":/icons/esign.png"));
    m_esignButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_esignButton->setPopupMode(QToolButton::InstantPopup);
    m_esignButton->setStyleSheet(
        "QToolButton::menu-indicator {"
        " subcontrol-position: bottom right;"
        " right: 0px; bottom: 0px;"
        " width: 0px; height: 0px;"
        "}");
    m_esignButton->setToolTip(
        "Add a visible electronic signature mark. This is not a certificate-based "
        "digital signature.");
    auto *eSignatureMenu = new QMenu(m_esignButton);
    m_esignTypeAction = eSignatureMenu->addAction("Type signature...");
    connect(m_esignTypeAction, &QAction::triggered,
            this, &MainWindow::addTypedESignature);
    m_esignDrawAction = eSignatureMenu->addAction("Draw signature...");
    connect(m_esignDrawAction, &QAction::triggered,
            this, &MainWindow::drawESignature);
    m_esignUploadAction = eSignatureMenu->addAction("Upload signature image...");
    connect(m_esignUploadAction, &QAction::triggered,
            this, &MainWindow::uploadESignature);
    eSignatureMenu->addSeparator();
    m_manageESignaturesAction = eSignatureMenu->addAction("Saved signatures...");
    connect(m_manageESignaturesAction, &QAction::triggered,
            this, &MainWindow::manageESignatures);
    m_esignButton->setMenu(eSignatureMenu);
    toolbar->addWidget(m_esignButton);

    m_signAction->setToolTip(
        "Sign a separate PDF copy using a certificate and private key.");
}
