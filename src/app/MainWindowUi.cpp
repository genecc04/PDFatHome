#include "MainWindow.h"
#include "AnnotationFonts.h"
#include "BookmarkPane.h"
#include "ColorMenuButton.h"
#include "PdfEditor.h"
#include "ReconstructionOverlay.h"
#include "SearchPane.h"
#include "ThumbnailPane.h"

#include <QAction>
#include <QActionGroup>
#include <QAbstractItemView>
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
#include <QFont>
#include <QFontComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QListWidget>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QPainter>
#include <QPixmap>
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

// The pages of the Tools pane, in the order they are added to its stack.
enum ToolsPage { kToolsPageNone = 0, kToolsPageText, kToolsPageDraw, kToolsPageRedaction,
                 kToolsPageImage };

// A letter drawn as a toolbar icon, so the B / I / U buttons need no image files.
QIcon styleIcon(const QString &letter, bool bold, bool italic, bool underline)
{
    const qreal dpr = qApp->devicePixelRatio();
    QPixmap pixmap(QSize(kToolbarIconSize, kToolbarIconSize) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::TextAntialiasing);
    QFont font = painter.font();
    font.setPixelSize(int(kToolbarIconSize * 0.62));
    font.setBold(bold);
    font.setItalic(italic);
    font.setUnderline(underline);
    painter.setFont(font);
    painter.setPen(qApp->palette().color(QPalette::ButtonText));
    painter.drawText(QRect(0, 0, kToolbarIconSize, kToolbarIconSize), Qt::AlignCenter, letter);
    painter.end();
    return QIcon(pixmap);
}

QToolBar *makeToolbar(QMainWindow *window, const QString &title,
                      const QString &objectName,
                      Qt::ToolButtonStyle buttonStyle = Qt::ToolButtonIconOnly,
                      bool highlightChecked = false)
{
    QToolBar *toolbar = window->addToolBar(title);
    toolbar->setObjectName(objectName);
    toolbar->setMovable(false);
    toolbar->setIconSize(QSize(kToolbarIconSize, kToolbarIconSize));
    toolbar->setToolButtonStyle(buttonStyle);
    if (highlightChecked) {
        toolbar->setStyleSheet(QStringLiteral(
            "QToolButton:checked { border: 2px solid palette(highlight); border-radius: 4px; }"));
    }
    return toolbar;
}
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
            bool underline = false;
            if (!m_editor || m_viewOnly
                || !m_editor->textAnnotationAt(page, point, annotationId, bounds, text,
                                               &fontName, &fontSize, &color, &underline))
                return false;

            style->family = AnnotationFonts::family(fontName);
            style->bold = AnnotationFonts::isBold(fontName);
            style->italic = AnnotationFonts::isItalic(fontName);
            style->underline = underline;
            style->sizePoints = fontSize;
            style->color = color;
            return true;
        });
    m_view->setImageStampHandler(
        [this](int page, const QPointF &point, PdfEditor::ImageStamp *stamp) {
            if (!m_editor || m_viewOnly)
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
    toolGroup->addAction(m_reconstructAction);
    editMenu->addAction(m_reconstructAction);
    editMenu->addAction(m_applyReconstructionAction);
    editMenu->addAction(m_discardReconstructionAction);

    m_addImageAction = editMenu->addAction("Add &image...");
    m_addImageAction->setShortcut(Qt::CTRL | Qt::Key_I);
    connect(m_addImageAction, &QAction::triggered, this, &MainWindow::addImage);

    m_viewMenu = menuBar()->addMenu("&View");
    QMenu *viewMenu = m_viewMenu;
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
    QToolBar *toolbar = makeToolbar(this, "Navigation", "navigationToolbar");

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

// Add image sits on the Tools toolbar. Crop, delete and cancel are only actions here;
// their buttons are on the Image page of the Tools pane (setupToolsPane).
void MainWindow::setupImageActions()
{
    m_addImageAction->setIcon(QIcon(":/icons/addimage.png"));
    m_addImageAction->setIconText("Add image"); // the toolbar label, without the "..."

    m_cropImageAction = new QAction(QIcon(":/icons/crop.png"), "Crop image", this);
    m_cropImageAction->setCheckable(true);
    m_cropImageAction->setEnabled(false);
    m_cropImageAction->setToolTip("Crop the image being placed: drag a corner inward");
    connect(m_cropImageAction, &QAction::toggled,
            m_view, &PageView::setImageCropMode);

    m_deleteImageAction = new QAction(QIcon(":/icons/deleteimage.png"),
                                      "Delete selected image", this);
    m_deleteImageAction->setEnabled(false);
    m_deleteImageAction->setToolTip("Delete selected added image");
    connect(m_deleteImageAction, &QAction::triggered,
            m_view, &PageView::deleteSelectedImageStamp);

    m_cancelImageAction = new QAction(QIcon(":/icons/cancelimage.png"),
                                      "Cancel placement", this);
    m_cancelImageAction->setEnabled(false);
    m_cancelImageAction->setToolTip("Stop placing the image (Esc)");
    m_cancelImageAction->setShortcut(QKeySequence(Qt::Key_Escape));
    m_cancelImageAction->setShortcutContext(Qt::WindowShortcut);
    addAction(m_cancelImageAction); // the shortcut works without a button on screen
    connect(m_cancelImageAction, &QAction::triggered,
            m_view, &PageView::cancelImagePlacement);
}

void MainWindow::setupToolsToolbar()
{
    // One toolbar holds the buttons that pick a tool. What a tool is set to (font,
    // ink, marked areas...) lives in the Tools pane on the right: setupToolsPane().
    QToolBar *toolbar = makeToolbar(this, "Tools", "toolsToolbar",
                                    Qt::ToolButtonTextBesideIcon, true);

    // Text
    toolbar->addAction(m_addTextAction);
    m_addTextAction->setIcon(QIcon(":/icons/addtext.png"));
    m_addTextAction->setToolTip("Add text to the PDF (active when checked)");

    m_deleteTextAction = toolbar->addAction("Delete");
    m_deleteTextAction->setIcon(QIcon(":/icons/deletetext.png"));
    m_deleteTextAction->setEnabled(false);
    m_deleteTextAction->setToolTip("Delete selected text box");
    connect(m_deleteTextAction, &QAction::triggered,
            m_view, &PageView::deleteSelectedTextAnnotation);
    if (QToolButton *button = qobject_cast<QToolButton *>(
            toolbar->widgetForAction(m_deleteTextAction))) {
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    }

    toolbar->addSeparator();

    // Drawing
    toolbar->addAction(m_drawAction);
    m_drawAction->setIcon(QIcon(":/icons/draw.png"));
    m_drawAction->setToolTip("Draw on the PDF (active when checked)");

    toolbar->addSeparator();

    // Adding an image. Crop, delete and cancel are on the Image page of the Tools pane.
    toolbar->addAction(m_addImageAction);

    toolbar->addSeparator();

    // Editing text that is already in the PDF. The menu keeps its long name; a
    // toolbar button shows the short "icon text" instead.
    m_reconstructAction->setIconText("Reconstruct");
    toolbar->addAction(m_reconstructAction);

    toolbar->addSeparator();

    // Redaction (the list of marked areas and the Apply button are in the pane)
    toolbar->addAction(m_redactAction);
    m_redactAction->setIcon(QIcon(":/icons/markredaction.png"));
    m_redactAction->setText("Mark redaction");
    m_redactAction->setToolTip(
        "Mark rectangular areas for removal from a separate redacted PDF copy");

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

// The Tools pane: a dock on the right with one page per tool. A QStackedWidget
// shows the page of whichever tool is active (updatePane below decides which);
// with no tool active it shows a short "select a tool" note.
//
// Nothing here is new logic. The text and drawing controls are the same ones that
// used to sit in two toolbars, with the same handlers, moved into form layouts.
void MainWindow::setupToolsPane()
{
    ReconstructionOverlay *overlay = m_view->reconstruction();

    // A widget put at the left of its form row instead of stretched across it.
    const auto leftAligned = [](QWidget *widget) {
        auto *row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        row->addWidget(widget);
        row->addStretch();
        return row;
    };

    // A push button that mirrors an action: clicking triggers it, and it is
    // enabled and disabled together with it. It never takes keyboard focus, so
    // pressing it does not count as leaving a text editor.
    const auto actionButton = [](QAction *action, const QString &text) {
        auto *button = new QPushButton(text);
        button->setToolTip(action->toolTip());
        button->setFocusPolicy(Qt::NoFocus);
        button->setEnabled(action->isEnabled());
        QObject::connect(button, &QPushButton::clicked, action, &QAction::trigger);
        QObject::connect(action, &QAction::changed, button, [button, action]() {
            button->setEnabled(action->isEnabled());
        });
        return button;
    };

    auto *stack = new QStackedWidget;
    stack->setMinimumWidth(230);

    // ---- page 0: no tool active ---------------------------------------------
    auto *nonePage = new QWidget;
    auto *noneLayout = new QVBoxLayout(nonePage);
    auto *noneLabel = new QLabel("Select a tool on the toolbar to see its options here.");
    noneLabel->setWordWrap(true);
    noneLabel->setAlignment(Qt::AlignCenter);
    noneLabel->setEnabled(false); // greyed text, whatever the theme
    noneLayout->addWidget(noneLabel);
    stack->addWidget(nonePage);

    // ---- page 1: text (also used by reconstruct mode) -------------------------
    auto *textPage = new QWidget;
    auto *textPageLayout = new QVBoxLayout(textPage);

    auto *textGroup = new QGroupBox("Text");
    // Moving focus to one of these controls must not count as leaving the text
    // editor (see the same check in PageView and ReconstructionOverlay). Both walk
    // up the parent chain, so marking the group covers everything inside it.
    textGroup->setProperty("textEditorStyleControl", true);
    auto *textForm = new QFormLayout(textGroup);

    // These controls style two things: new text boxes (and the text box being
    // edited), and the text of a reconstructed region while its editor is open.
    m_fontBox = new QComboBox;
    m_fontBox->addItems(AnnotationFonts::families());
    m_fontBox->setCurrentText(AnnotationFonts::families().first());
    m_fontBox->setFocusPolicy(Qt::NoFocus);
    m_fontBox->setProperty("textEditorStyleControl", true);
    m_fontBox->setToolTip("Font");

    // While a reconstructed region is being edited, this box replaces the one above
    // and offers every installed font. The text box tool keeps Helvetica/Times/Courier
    // because a FreeText annotation can only name a standard PDF font.
    auto *reconFontBox = new QFontComboBox;
    reconFontBox->setEditable(false);
    reconFontBox->setFontFilters(QFontComboBox::ScalableFonts);
    reconFontBox->setMaxVisibleItems(10);
    reconFontBox->setFocusPolicy(Qt::NoFocus);
    reconFontBox->setProperty("textEditorStyleControl", true);
    reconFontBox->setToolTip("Font of the selected text");

    // Open each drop-down list below its box. The default lays the list over the box
    // with the current item on top, which for a long font list covered most of the pane.
    const QString dropDownStyle = QStringLiteral("QComboBox { combobox-popup: 0; }");
    m_fontBox->setStyleSheet(dropDownStyle);
    reconFontBox->setStyleSheet(dropDownStyle);

    // Both boxes share one form row and only one is visible at a time. (A
    // QStackedWidget here stretched the box to the height of the whole pane: a stacked
    // layout ignores the size policy of what it holds.)
    auto *fontRow = new QVBoxLayout;
    fontRow->setContentsMargins(0, 0, 0, 0);
    fontRow->addWidget(m_fontBox);
    fontRow->addWidget(reconFontBox);
    reconFontBox->setVisible(false);
    textForm->addRow("Font:", fontRow);

    m_fontSizeBox = new QSpinBox;
    m_fontSizeBox->setRange(6, 200);
    m_fontSizeBox->setValue(12);
    m_fontSizeBox->setSuffix(" pt");
    m_fontSizeBox->setMinimumWidth(kNumericControlWidth);
    m_fontSizeBox->setCorrectionMode(QAbstractSpinBox::CorrectToNearestValue);
    m_fontSizeBox->setKeyboardTracking(false);
    m_fontSizeBox->setProperty("textEditorStyleControl", true);
    m_fontSizeBox->setToolTip("Font size");
    textForm->addRow("Size:", leftAligned(m_fontSizeBox));

    m_textColorButton = new ColorMenuButton(QColor(0, 0, 0));
    m_textColorButton->setProperty("textEditorStyleControl", true);
    textForm->addRow("Color:", leftAligned(m_textColorButton));

    m_boldAction = new QAction(styleIcon("B", true, false, false), "Bold", this);
    m_italicAction = new QAction(styleIcon("I", false, true, false), "Italic", this);
    m_underlineAction = new QAction(styleIcon("U", false, false, true), "Underline", this);
    m_boldAction->setToolTip("Bold (Ctrl+B while editing text)");
    m_italicAction->setToolTip("Italic (Ctrl+I while editing text)");
    m_underlineAction->setToolTip("Underline (Ctrl+U while editing text)");

    auto *styleRow = new QHBoxLayout;
    styleRow->setContentsMargins(0, 0, 0, 0);
    for (QAction *action : { m_boldAction, m_italicAction, m_underlineAction }) {
        action->setCheckable(true);
        auto *button = new QToolButton;
        button->setDefaultAction(action);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setIconSize(QSize(26, 26));
        button->setProperty("textEditorStyleControl", true);
        // Same "pressed in" look the toolbar used for a checked button.
        button->setStyleSheet(QStringLiteral(
            "QToolButton:checked { border: 2px solid palette(highlight); border-radius: 4px; }"));
        styleRow->addWidget(button);
    }
    styleRow->addStretch();
    textForm->addRow("Style:", styleRow);

    textPageLayout->addWidget(textGroup);

    // Only visible in reconstruct mode: what to do with the edits.
    auto *reconstructSection = new QGroupBox("Reconstruct");
    auto *reconstructLayout = new QVBoxLayout(reconstructSection);
    auto *reconstructHint = new QLabel(
        "Click a text region on the page to edit it. Edits wait until you apply them. "
        "The text options above work while a region is open.");
    reconstructHint->setWordWrap(true);
    reconstructLayout->addWidget(reconstructHint);
    auto *pendingLabel = new QLabel("No edits waiting.");
    reconstructLayout->addWidget(pendingLabel);
    reconstructLayout->addWidget(
        actionButton(m_applyReconstructionAction, "Apply reconstruction"));
    reconstructLayout->addWidget(
        actionButton(m_discardReconstructionAction, "Discard edits"));
    textPageLayout->addWidget(reconstructSection);

    textPageLayout->addStretch();
    stack->addWidget(textPage);

    // ---- page 2: drawing --------------------------------------------------------
    auto *drawPage = new QWidget;
    auto *drawPageLayout = new QVBoxLayout(drawPage);
    auto *drawGroup = new QGroupBox("Drawing");
    auto *drawForm = new QFormLayout(drawGroup);

    auto *mode = new QComboBox;
    mode->addItem("Pen");
    mode->addItem("Highlighter");
    mode->addItem("White-out");
    mode->addItem("Eraser");
    mode->setFocusPolicy(Qt::NoFocus);
    mode->setStyleSheet(dropDownStyle);
    drawForm->addRow("Tool:", mode);

    auto *size = new QSpinBox;
    size->setRange(1, 60);
    size->setValue(2);
    size->setSuffix(" pt");
    size->setMinimumWidth(kNumericControlWidth);
    size->setCorrectionMode(QAbstractSpinBox::CorrectToNearestValue);
    size->setKeyboardTracking(false);
    drawForm->addRow("Size:", leftAligned(size));

    auto *opacity = new QSpinBox;
    opacity->setRange(5, 100);
    opacity->setValue(100);
    opacity->setSuffix(" %");
    opacity->setMinimumWidth(kNumericControlWidth);
    opacity->setCorrectionMode(QAbstractSpinBox::CorrectToNearestValue);
    opacity->setKeyboardTracking(false);
    drawForm->addRow("Opacity:", leftAligned(opacity));

    auto *colorButton = new ColorMenuButton(QColor(0, 0, 0));
    drawForm->addRow("Color:", leftAligned(colorButton));

    drawPageLayout->addWidget(drawGroup);
    drawPageLayout->addStretch();
    stack->addWidget(drawPage);

    // ---- page 3: redaction ------------------------------------------------------
    auto *redactionPage = new QWidget;
    auto *redactionPageLayout = new QVBoxLayout(redactionPage);
    auto *redactionGroup = new QGroupBox("Redaction");
    auto *redactionLayout = new QVBoxLayout(redactionGroup);

    auto *redactionHint = new QLabel(
        "Drag over content to mark it for removal. Applying saves a separate redacted "
        "copy; the original is not changed.");
    redactionHint->setWordWrap(true);
    redactionLayout->addWidget(redactionHint);

    auto *redactionList = new QListWidget;
    redactionList->setSelectionMode(QAbstractItemView::SingleSelection);
    redactionList->setToolTip("The marked areas. Select one to jump to it.");
    redactionLayout->addWidget(redactionList, 1);

    auto *deleteAreaButton = new QPushButton("Delete selected");
    auto *clearAreasButton = new QPushButton("Clear all");
    deleteAreaButton->setEnabled(false);
    clearAreasButton->setEnabled(false);
    auto *areaButtons = new QHBoxLayout;
    areaButtons->addWidget(deleteAreaButton);
    areaButtons->addWidget(clearAreasButton);
    redactionLayout->addLayout(areaButtons);

    m_applyRedactionsAction->setIcon(QIcon(":/icons/applyredaction.png"));
    m_applyRedactionsAction->setText("Apply redaction");
    m_applyRedactionsAction->setToolTip(
        "Apply marked redactions and save a separate PDF copy");
    redactionLayout->addWidget(actionButton(m_applyRedactionsAction, "Apply redaction..."));

    redactionPageLayout->addWidget(redactionGroup);
    stack->addWidget(redactionPage);

    // ---- page 4: image ----------------------------------------------------------
    // Shown while an image (or an E-sign mark) is being placed, and while an added
    // image is selected. The buttons are the actions that used to be on the Image toolbar.
    auto *imagePage = new QWidget;
    auto *imagePageLayout = new QVBoxLayout(imagePage);
    auto *imageGroup = new QGroupBox("Image");
    auto *imageLayout = new QVBoxLayout(imageGroup);

    auto *imageHint = new QLabel(
        "Drag the image to move it, or a corner to resize it. While placing: Enter places "
        "it and Esc cancels.");
    imageHint->setWordWrap(true);
    imageLayout->addWidget(imageHint);

    for (QAction *action : { m_cropImageAction, m_deleteImageAction, m_cancelImageAction }) {
        auto *button = new QToolButton;
        button->setDefaultAction(action);
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setIconSize(QSize(24, 24));
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        // Crop stays "pressed in" while cropping.
        button->setStyleSheet(QStringLiteral(
            "QToolButton:checked { border: 2px solid palette(highlight); border-radius: 4px; }"));
        imageLayout->addWidget(button);
    }

    imagePageLayout->addWidget(imageGroup);
    imagePageLayout->addStretch();
    stack->addWidget(imagePage);

    // ---- the dock -------------------------------------------------------------------
    // The pane sits in a scroll area so a short window can still reach everything.
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(stack);

    m_toolsDock = new QDockWidget("Tools", this);
    m_toolsDock->setObjectName("toolsDock");
    m_toolsDock->setWidget(scroll);
    m_toolsDock->setMinimumWidth(240);
    m_toolsDock->setFeatures(QDockWidget::DockWidgetClosable |
                             QDockWidget::DockWidgetMovable);
    addDockWidget(Qt::RightDockWidgetArea, m_toolsDock);
    resizeDocks({ m_toolsDock }, { 260 }, Qt::Horizontal);

    // Show the dock's open/closed toggle in the View menu, after Pages and Bookmarks.
    if (m_viewMenu) {
        const QList<QAction *> viewActions = m_viewMenu->actions();
        m_viewMenu->insertAction(viewActions.value(2), m_toolsDock->toggleViewAction());
    }

    // ---- which page is showing --------------------------------------------------------
    // Reads the state of the tools and picks the page. Runs whenever a tool is
    // switched on or off, a text editor opens or closes, or the marked areas change.
    const auto updatePane = [this, stack, overlay, textGroup, reconstructSection]() {
        const bool reconstructing = m_reconstructAction->isChecked();

        int page = kToolsPageNone;
        if (reconstructing || m_addTextAction->isChecked() || m_view->isTextEditorOpen())
            page = kToolsPageText;
        else if (m_drawAction->isChecked())
            page = kToolsPageDraw;
        else if (m_view->hasActiveImagePlacement() || m_view->hasSelectedImageStamp())
            page = kToolsPageImage;
        else if (m_redactAction->isChecked() || m_view->hasRedactionAreas())
            page = kToolsPageRedaction;

        stack->setCurrentIndex(page);
        reconstructSection->setVisible(reconstructing);

        // In reconstruct mode these controls style the text of the region being
        // edited, so they only mean something while an editor is open.
        textGroup->setEnabled(!reconstructing || overlay->isEditing());
    };

    for (QAction *action : { m_addTextAction, m_drawAction, m_redactAction,
                             m_reconstructAction }) {
        connect(action, &QAction::toggled, this, [updatePane](bool) { updatePane(); });
    }
    // The redaction tool can switch itself off (Esc) with its action's signals
    // blocked, so it is watched from the view as well.
    connect(m_view, &PageView::redactionToolChanged, this,
            [updatePane](bool) { updatePane(); });
    // An image starts or stops being placed, or an added image is selected or not.
    connect(m_view, &PageView::imagePlacementStateChanged, this,
            [updatePane](bool, bool) { updatePane(); });
    connect(m_view, &PageView::imageStampSelectionChanged, this,
            [updatePane](bool) { updatePane(); });
    // Also emitted when a text editor opens or closes.
    connect(m_view, &PageView::pendingChangesChanged, this, [updatePane]() { updatePane(); });
    connect(overlay, &ReconstructionOverlay::editingChanged, this,
            [updatePane](bool) { updatePane(); });

    // ---- text controls (the handlers the text toolbar used to have) ---------------------
    // Text box style: read all controls into one style and hand it to the view.
    auto applyTextBoxStyle = [this]() {
        m_textBoxStyle.family = m_fontBox->currentText();
        m_textBoxStyle.sizePoints = m_fontSizeBox->value();
        m_textBoxStyle.color = m_textColorButton->color();
        m_textBoxStyle.bold = m_boldAction->isChecked();
        m_textBoxStyle.italic = m_italicAction->isChecked();
        m_textBoxStyle.underline = m_underlineAction->isChecked();
        m_view->setTextStyle(m_textBoxStyle);
    };

    // Show the text box style in the controls without triggering their handlers.
    auto showTextBoxStyle = [this]() {
        const QSignalBlocker fontBlocker(m_fontBox);
        const QSignalBlocker sizeBlocker(m_fontSizeBox);
        const QSignalBlocker colorBlocker(m_textColorButton);
        const QSignalBlocker boldBlocker(m_boldAction);
        const QSignalBlocker italicBlocker(m_italicAction);
        const QSignalBlocker underlineBlocker(m_underlineAction);
        m_fontBox->setCurrentText(m_textBoxStyle.family);
        m_fontSizeBox->setValue(qRound(m_textBoxStyle.sizePoints));
        m_textColorButton->setColor(m_textBoxStyle.color);
        m_boldAction->setChecked(m_textBoxStyle.bold);
        m_italicAction->setChecked(m_textBoxStyle.italic);
        m_underlineAction->setChecked(m_textBoxStyle.underline);
    };

    // A change goes to the open reconstruct editor if there is one (and only
    // that one property, so a mixed selection keeps its other styles); otherwise
    // it is the style for text boxes. The font is the exception: the text box font
    // box (Helvetica/Times/Courier) is hidden while a reconstruct editor is open,
    // and the all-fonts box takes its place.
    connect(m_fontBox, &QComboBox::currentTextChanged, this,
            [applyTextBoxStyle](const QString &) { applyTextBoxStyle(); });

    // textActivated fires only for the user's own choice, so picking the font that
    // is already shown still applies it (the PDF's font may not be installed, and
    // the editor then shows a substitute).
    connect(reconFontBox, &QComboBox::textActivated, this,
            [overlay](const QString &family) {
        if (overlay->isEditing())
            overlay->setFontName(family);
    });
    connect(m_fontSizeBox, &QSpinBox::valueChanged, this,
            [overlay, applyTextBoxStyle](int points) {
        if (overlay->isEditing())
            overlay->setFontSize(points);
        else
            applyTextBoxStyle();
    });
    connect(m_textColorButton, &ColorMenuButton::colorChanged, this,
            [overlay, applyTextBoxStyle](const QColor &color) {
        if (overlay->isEditing())
            overlay->setTextColor(color);
        else
            applyTextBoxStyle();
    });
    connect(m_boldAction, &QAction::toggled, this, [overlay, applyTextBoxStyle](bool on) {
        if (overlay->isEditing())
            overlay->setBold(on);
        else
            applyTextBoxStyle();
    });
    connect(m_italicAction, &QAction::toggled, this, [overlay, applyTextBoxStyle](bool on) {
        if (overlay->isEditing())
            overlay->setItalic(on);
        else
            applyTextBoxStyle();
    });
    connect(m_underlineAction, &QAction::toggled, this, [overlay, applyTextBoxStyle](bool on) {
        if (overlay->isEditing())
            overlay->setUnderlined(on);
        else
            applyTextBoxStyle();
    });

    // The reconstruct editor tells the pane what the text at the cursor looks like,
    // and when it opens or closes.
    connect(overlay, &ReconstructionOverlay::formatChanged, this,
            [this, reconFontBox](const ReconstructionFormat &format) {
        const QSignalBlocker sizeBlocker(m_fontSizeBox);
        const QSignalBlocker colorBlocker(m_textColorButton);
        const QSignalBlocker boldBlocker(m_boldAction);
        const QSignalBlocker italicBlocker(m_italicAction);
        const QSignalBlocker underlineBlocker(m_underlineAction);
        if (!format.fontFamily.isEmpty())
            reconFontBox->setCurrentFont(QFont(format.fontFamily));
        m_fontSizeBox->setValue(qRound(format.sizePoints));
        m_textColorButton->setColor(format.color);
        m_boldAction->setChecked(format.bold);
        m_italicAction->setChecked(format.italic);
        m_underlineAction->setChecked(format.underlined);
    });
    connect(overlay, &ReconstructionOverlay::editingChanged, this,
            [this, reconFontBox, showTextBoxStyle](bool editing) {
        m_fontBox->setVisible(!editing);
        reconFontBox->setVisible(editing);
        if (!editing)
            showTextBoxStyle(); // back to the style for new text boxes
    });
    connect(overlay, &ReconstructionOverlay::pendingChanged, this,
            [pendingLabel](int count) {
        pendingLabel->setText(count > 0
                                  ? QString("%1 edit(s) waiting to be applied.").arg(count)
                                  : QStringLiteral("No edits waiting."));
    });

    // Editing an existing text box shows that box's style.
    connect(m_view, &PageView::textAnnotationEditingStyleChanged, this,
            [this](const PageView::TextStyle &style) {
        const QSignalBlocker fontBlocker(m_fontBox);
        const QSignalBlocker sizeBlocker(m_fontSizeBox);
        const QSignalBlocker colorBlocker(m_textColorButton);
        const QSignalBlocker boldBlocker(m_boldAction);
        const QSignalBlocker italicBlocker(m_italicAction);
        const QSignalBlocker underlineBlocker(m_underlineAction);
        m_fontBox->setCurrentText(style.family);
        m_fontSizeBox->setValue(qRound(style.sizePoints));
        m_textColorButton->setColor(style.color);
        m_boldAction->setChecked(style.bold);
        m_italicAction->setChecked(style.italic);
        m_underlineAction->setChecked(style.underline);
    });

    // ---- drawing controls (the handlers the draw toolbar used to have) ------------------
    auto applyDrawStyle = [this, size, opacity, colorButton]() {
        PageView::DrawStyle style;
        style.color = colorButton->color();
        style.widthPoints = size->value();
        style.opacity = opacity->value() / 100.0;
        m_view->setDrawStyle(style);
    };
    connect(size, &QSpinBox::valueChanged, this, applyDrawStyle);
    connect(opacity, &QSpinBox::valueChanged, this, applyDrawStyle);
    connect(colorButton, &ColorMenuButton::colorChanged, this, applyDrawStyle);

    connect(mode, &QComboBox::currentIndexChanged, this,
            [this, size, opacity, colorButton, applyDrawStyle](int index) {
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
                applyDrawStyle();
            });

    // ---- the list of marked redaction areas ---------------------------------------------
    // Rebuilt from the view every time the areas change. Nothing is selected after a
    // rebuild, so the view's highlight is cleared with it.
    const auto refreshRedactionList = [this, redactionList, deleteAreaButton,
                                       clearAreasButton]() {
        const QVector<PdfEditor::RedactionArea> areas = m_view->redactionAreas();
        {
            const QSignalBlocker blocker(redactionList);
            redactionList->clear();
            for (const PdfEditor::RedactionArea &area : areas) {
                redactionList->addItem(QString("Page %1: %2 x %3 pt")
                                           .arg(area.page + 1)
                                           .arg(qRound(area.bounds.width()))
                                           .arg(qRound(area.bounds.height())));
            }
        }
        deleteAreaButton->setEnabled(false);
        clearAreasButton->setEnabled(!areas.isEmpty());
        m_view->setHighlightedRedactionArea(-1);
    };

    connect(m_view, &PageView::redactionAreasChanged, this,
            [refreshRedactionList, updatePane](int) {
        refreshRedactionList();
        updatePane();
    });

    // Selecting an area highlights it on the page and scrolls to it.
    connect(redactionList, &QListWidget::currentRowChanged, this,
            [this, deleteAreaButton](int row) {
        deleteAreaButton->setEnabled(row >= 0);
        m_view->setHighlightedRedactionArea(row);

        const QVector<PdfEditor::RedactionArea> areas = m_view->redactionAreas();
        if (row >= 0 && row < areas.size())
            m_view->jumpToPage(areas.at(row).page, areas.at(row).bounds.center());
    });
    connect(deleteAreaButton, &QPushButton::clicked, this, [this, redactionList]() {
        m_view->removeRedactionArea(redactionList->currentRow());
    });
    connect(clearAreasButton, &QPushButton::clicked,
            m_view, &PageView::clearRedactionAreas);

    // ---- start state ----------------------------------------------------------------------
    applyTextBoxStyle();
    applyDrawStyle();
    refreshRedactionList();
    updatePane();
}