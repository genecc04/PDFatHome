#pragma once
#include <QAbstractScrollArea>
#include <QCache>
#include <QColor>
#include <QImage>
#include <QList>
#include <QPixmap>
#include <QPair>
#include <QPointF>
#include <QPolygonF>
#include <QRect>
#include <QRectF>
#include <QSet>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QUrl>
#include <QVector>
#include <QThreadPool>
#include <array>
#include <functional>

#include "PdfEditor.h"
#include "DocumentSource.h"
#include "RasterStore.h"

class QComboBox;
class QLineEdit;
class QPainterPath;
class QPlainTextEdit;
class QPushButton;
class SearchResultModel;
class QTimer;

class PageView : public QAbstractScrollArea
{
    Q_OBJECT
public:
    enum class ZoomMode { Custom, FitToWidth, FitInView };

    struct TextStyle {
        QString family = QStringLiteral("Helvetica");
        double sizePoints = 12.0;
        QColor color = QColor(0, 0, 0);
    };

    struct DrawStyle {
        QColor color = QColor(0, 0, 0);
        double widthPoints = 2.0;
        double opacity = 1.0;
    };

    struct Stroke {
        int page = -1;
        QVector<QPointF> points;
        DrawStyle style;
    };

    struct FormField {
        enum class Type { None, Text, CheckBox, RadioButton, ComboBox, ListBox };

        Type type = Type::None;
        QRectF bounds;
        QString value;
        QStringList options;
        QStringList optionValues;
        bool checked = false;
        bool readOnly = false;
        bool multiline = false;
        bool password = false;
        bool editableChoice = false;
        int widgetIndex = -1;
    };

    explicit PageView(QWidget *parent = nullptr);
    ~PageView() override;

    // pageSizes, when its length matches the page count, saves asking the document for them.
    void setDocument(PdfEditor *document, bool preserveRasterPreviews = false,
                     const QVector<QSizeF> &pageSizes = {});
    void setLoading(bool loading);
    void setInteractionBlocked(bool blocked);
    QVector<QSizeF> pageSizes() const { return m_pageSizes; }
    QPair<int, int> visiblePageRange() const;
    void stepLoading();
    void setRenderSource(const DocumentSource &source);
    void invalidatePageRenders(const QList<int> &changedPages = {});
    void setSearchModel(SearchResultModel *model);
    void setCurrentSearchResultIndex(int index);

    int currentPage() const { return m_currentPage; }
    bool hasSelectedTextAnnotation() const { return m_selectedTextAnnotationPage >= 0; }
    QRectF selectedTextAnnotationBounds() const { return m_selectedTextAnnotationBounds; }
    bool hasSelectedImageStamp() const { return m_selectedImagePage >= 0; }
    bool hasActiveImagePlacement() const { return m_imageAwaiting || m_imageActive; }
    bool hasPositionedImage() const { return m_imageActive; }
    void jumpToPage(int page, const QPointF &location = QPointF());

    void setZoomMode(ZoomMode mode);
    ZoomMode zoomMode() const { return m_zoomMode; }
    void setZoomFactor(double factor);
    double zoomFactor() const;

    void setTextToolActive(bool active);
    void setTextStyle(const TextStyle &style);
    void setFormFieldHandler(
        const std::function<bool(int, const QPointF &, FormField *)> &handler);
    void setFormFieldValueHandler(
        const std::function<bool(int, int, const QString &)> &handler);
    void setTextAnnotationHandler(
        const std::function<bool(int, const QPointF &, int *, QRectF *, QString *,
                                 TextStyle *)> &handler);
    void setImageStampHandler(
        const std::function<bool(int, const QPointF &, PdfEditor::ImageStamp *)> &handler);
    void refreshSelectedTextAnnotation();
    void editTextAnnotationInline(int page, int annotationId, const QString &text);
    void deleteSelectedTextAnnotation();
    void deleteSelectedImageStamp();
    void setFormFields(int page, const QVector<FormField> &fields);
    void clearFormFieldOverlays();
    bool hasPendingChanges() const;
    void setImageCropMode(bool active);
    void cancelImagePlacement();

    void setDrawToolActive(bool active);
    void setDrawStyle(const DrawStyle &style);
    void setRedactionToolActive(bool active);
    void setSignatureToolActive(bool active);
    bool hasRedactionAreas() const { return !m_redactionAreas.isEmpty(); }
    QVector<PdfEditor::RedactionArea> redactionAreas() const { return m_redactionAreas; }
    void clearRedactionAreas();
    void setEraserActive(bool active);
    void setInkStrokeHandler(
        const std::function<bool(int, QVector<Stroke> *)> &handler);
    bool hasPendingStrokes() const { return !m_strokes.isEmpty(); }
    bool undoLastStroke();

    void beginImagePlacement(const QImage &image);

    void commitPendingEdits();
    void preserveCommittedStrokes(const QVector<PageView::Stroke> &strokes);
    void preserveErasedStrokes(int page, const QVector<PageView::Stroke> &strokes);
    void preserveTextAnnotationChange(int page, const QRectF &oldBounds,
                                      const QRectF &newBounds, const QString &text,
                                      const PageView::TextStyle &style);

    bool hasTextSelection() const { return !m_selection.isEmpty(); }
    void copySelection();
    void selectAllText();
    void clearSelection();

signals:
    void currentPageChanged(int page);
    void visiblePageRangeChanged(int firstPage, int lastPage);
    void textPlaced(int page, const QRectF &boxPoints, const QString &text,
                    const PageView::TextStyle &style);
    void textToolFinished();
    void strokesPlaced(const QVector<PageView::Stroke> &strokes);
    void eraseRequested(int page, const QVector<QPointF> &path, double radius);
    void pendingStrokesChanged();
    void pendingChangesChanged();
    void textAnnotationEditRequested(int page, int annotationId, const QString &text);
    void textAnnotationEditingStyleChanged(const PageView::TextStyle &style);
    void textAnnotationEdited(int page, int annotationId,
                              const QString &oldText, const QString &newText,
                              const PageView::TextStyle &style);
    void textAnnotationDeleteRequested(int page, int annotationId, const QString &text);
    void textAnnotationBoundsChanged(int page, int annotationId, const QString &text,
                                     const QRectF &oldBounds, const QRectF &newBounds,
                                     const PageView::TextStyle &style);
    void textAnnotationSelectionChanged(bool selected);
    void imageStampBoundsChanged(int page, const QString &id,
                                 const QRectF &oldBounds, const QRectF &newBounds);
    void imageStampDeleteRequested(int page, const QString &id);
    void imageStampSelectionChanged(bool selected);
    void imagePlaced(int page, const QRectF &boxPoints, const QImage &image);
    void imagePlacementStateChanged(bool active, bool positioned);
    void imageCropModeChanged(bool active);
    void redactionAreasChanged(int count);
    void redactionToolChanged(bool active);
    void signaturePlacementRequested(int page, const QRectF &bounds);
    void signatureToolChanged(bool active);
    void textSelected();
    void openRequested();
    void externalLinkRequested(const QUrl &url);
    void linkHovered(const QUrl &url);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum class ImageDrag { None, Move, Resize };

    struct ErasePreviewStroke {
        Stroke stroke;
        QRectF bounds;
        bool erased = false;
    };

    struct TextAnnotationPreview {
        int page = -1;
        QRectF oldBounds;
        QRectF newBounds;
        QString text;
        TextStyle style;
    };

    struct ImageStampPreview {
        int page = -1;
        QRectF oldBounds;
        QRectF newBounds;
        QImage image;
    };

    struct SelectionPart {
        int page = -1;
        QList<QPolygonF> bounds;
        QString text;
        // The range this part covers on its page. Parts far from the viewport
        // are left unresolved until they are painted or copied.
        QPointF start;
        QPointF end;
        bool resolved = true;
    };

    struct FormFieldOverlay {
        int page = -1;
        FormField field;
    };

    struct PageLinkCache {
        QVector<PdfEditor::Link> links;
        bool loaded = false;
    };

    void onDocumentChanged(bool preserveRasterPreviews, const QVector<QSizeF> &pageSizes);
    void markCachedRastersStale();
    void invalidatePendingPageRenders();
    void discardQueuedPageRenders();
    void trimPageImageCaches();
    void enforcePageRasterBudget();
    void reserveTileCache(int visibleTiles);
    void recalculateLayout(bool keepPosition);
    double fitPageZoomFactor() const;
    void positionEmptyStateButton();
    QRect loadingIndicatorRect() const;
    void drawLoadingIndicator(QPainter &painter);
    void updateCurrentPage();
    void updateVisiblePageRange();
    int pageAtViewportPoint(const QPoint &point) const;
    const QVector<PdfEditor::Link> &linksOnPage(int page);
    void loadPageLinks(int page);
    PdfEditor::Link linkAtViewportPoint(const QPoint &point);
    void setHoveredLink(const QUrl &url);
    int nearestPage(const QPoint &viewportPos) const;
    QRect pageRectInViewport(int page) const;
    QPointF pagePointFor(int page, const QPoint &viewportPos) const;
    QPixmap pageImage(int page, const QSize &size, qreal dpr,
                      bool includeAnnotations = true);
    QPixmap pageTile(int page, const QSize &fullSize, int tileX, int tileY,
                     int tileSize, qreal dpr, bool includeAnnotations);
    const QPixmap *rasterFor(RasterStore &store, const RasterKey &key,
                             const QSize &renderSize, const QRectF &pageRegion,
                             qreal dpr);
    void finishRasterRender(RasterStore &store, const RasterKey &key, qreal dpr,
                            const QImage &image, const QString &error);
    std::array<RasterStore *, 3> rasterStores();
    int rasterCostKilobytes() const;
    void drawTiledPage(QPainter &painter, int page, const QRect &target,
                       const QRect &exposed, qreal dpr);
    void updateErasePreview(const QPointF &start, const QPointF &end,
                            QRectF &changedBounds);
    QRect erasePreviewViewportRect(const QRectF &bounds) const;
    void drawSearchResults(QPainter &painter, int page, const QRect &pageRect,
                           const PdfEditor::SearchResult &current);
    void drawFormFieldOverlays(QPainter &painter, int page, const QRect &pageRect);
    void drawStroke(QPainter &painter, const Stroke &stroke, const QRect &pageRect);
    void drawEmptyState(QPainter &painter, const QRect &exposed);
    void drawPageRaster(QPainter &painter, int page, const QRect &target,
                        const QRect &exposed, qreal dpr);
    void drawPageOverlays(QPainter &painter, int page, const QRect &target, qreal dpr);
    void drawSelectionHighlight(QPainter &painter, int page, const QRect &target);
    void drawTextAnnotationPreview(QPainter &painter, int page, const QRect &target,
                                   qreal dpr, const QRectF &oldBounds,
                                   const QRectF &newBounds, const QString &text,
                                   const TextStyle &style);
    void drawImageStampPreview(QPainter &painter, int page, const QRect &target,
                               qreal dpr, const QRectF &oldBounds,
                               const QRectF &newBounds, const QImage &image);
    void appendErasedInk(QPainterPath &erasedInk, const QVector<Stroke> &strokes) const;
    QRectF pageToViewport(const QRect &target, const QRectF &pageBounds) const;
    void finishPendingRasterPreview(int page, const QSize &tilePageSize = {});

    void startEditor(int page, const QRectF &boxPoints);
    void startAnnotationEditor(int page, int annotationId, const QRectF &boxPoints,
                               const QString &text, const TextStyle &style);
    void positionEditor();
    void applyEditorStyle();
    void finishEditor(bool commit);
    void startFormEditor(int page, const FormField &field);
    void positionFormEditor();
    void finishFormEditor(bool commit);
    QString formEditorValue() const;
    void commitStrokes();

    void finishImage(bool commit);
    void drawImagePlacement(QPainter &painter, const QRect &pageRect);
    QRectF imagePixelRect() const;
    void updateImageCropRect(const QPoint &viewportPos);
    void applyImageCrop();
    QRectF imageCropPixelRect() const;
    int imageCropHandleAt(const QPoint &viewportPos) const;
    int imageHandleAt(const QPoint &viewportPos) const;
    void updateImageCursor(const QPoint &viewportPos);

    void updateSelection();
    void resolveSelectionPart(SelectionPart &part);
    void extendSelection(const QPoint &viewportPos);
    void selectWordAt(int page, const QPointF &point);
    void selectTextAnnotation(int page, int annotationId, const QRectF &bounds,
                              const QString &text, const TextStyle &style);
    void clearSelectedTextAnnotation();
    QRectF selectedTextAnnotationViewportBounds() const;
    int textAnnotationResizeHandleAt(const QPoint &pos) const;
    void selectImageStamp(int page, const PdfEditor::ImageStamp &stamp);
    void clearSelectedImageStamp();
    QRectF selectedImageViewportBounds() const;
    int imageStampResizeHandleAt(const QPoint &pos) const;

    PdfEditor *m_document = nullptr;
    DocumentSource m_renderSource;
    quint64 m_renderGeneration = 0;
    bool m_renderRequestsSaturated = false;
    QThreadPool m_renderPool;
    QVector<PageLinkCache> m_pageLinkCaches;
    SearchResultModel *m_search = nullptr;
    QPushButton *m_emptyStateOpenButton = nullptr;
    QTimer *m_zoomTimer = nullptr;
    QPixmap m_loadingSprite;
    QTimer *m_loadingTimer = nullptr;
    bool m_loading = false;
    bool m_interactionBlocked = false;
    int m_loadingFrame = 0;
    int m_searchIndex = -1;

    ZoomMode m_zoomMode = ZoomMode::FitToWidth;
    double m_customZoom = 1.0;
    double m_pendingZoomFactor = 1.0;
    double m_scale = 1.0;
    QVector<QSizeF> m_pageSizes;
    double m_maxPageWidth = 1.0;
    double m_maxPageHeight = 1.0;
    QVector<QRect> m_pageRects;
    QSize m_documentSize;
    int m_currentPage = 0;
    int m_visibleFirstPage = -1;
    int m_visibleLastPage = -1;
    bool m_jumping = false;
    int m_pressedLinkPage = -1;
    QPoint m_linkPressPos;
    QUrl m_pressedLinkUrl;
    int m_pressedLinkDestinationPage = -1;
    QPointF m_pressedLinkLocation;
    QUrl m_hoveredLinkUrl;

    RasterStore m_pageStore;
    RasterStore m_baseStore;
    RasterStore m_tileStore;
    int m_sharedRasterBudgetKilobytes = 0;

    // text tool
    bool m_textTool = false;
    bool m_dragging = false;
    QPoint m_dragStart;
    QRect m_dragRect;
    QPlainTextEdit *m_editor = nullptr;
    int m_editorPage = -1;
    QRectF m_editorBox;
    TextStyle m_style;
    TextStyle m_editorStyle;
    bool m_editingTextAnnotation = false;
    int m_editingTextAnnotationId = 0;
    QString m_editingTextAnnotationOriginal;
    bool m_editingTextAnnotationStyleChanged = false;

    // draw tool
    bool m_drawTool = false;
    bool m_drawing = false;
    bool m_eraserActive = false;
    bool m_erasing = false;
    int m_erasePage = -1;
    QPoint m_eraserCursorPos;
    QVector<QPointF> m_erasePoints;
    QVector<ErasePreviewStroke> m_erasePreviewStrokes;
    QVector<Stroke> m_pendingCommittedStrokes;
    int m_pendingErasedPage = -1;
    QVector<Stroke> m_pendingErasedStrokes;
    TextAnnotationPreview m_pendingTextAnnotationPreview;
    ImageStampPreview m_pendingImageStampPreview;
    DrawStyle m_drawStyle;
    std::function<bool(int, QVector<Stroke> *)> m_inkStrokeHandler;
    Stroke m_activeStroke;
    QVector<Stroke> m_strokes;

    bool m_redactionTool = false;
    bool m_redactionDrawing = false;
    int m_redactionPage = -1;
    QPointF m_redactionStart;
    QRectF m_redactionPreview;
    QVector<PdfEditor::RedactionArea> m_redactionAreas;

    bool m_signatureTool = false;
    bool m_signatureDrawing = false;
    int m_signaturePage = -1;
    QPointF m_signatureStart;
    QRectF m_signaturePreview;

    // image placement
    QImage m_pendingImage;
    bool m_imageAwaiting = false;
    bool m_imageActive = false;
    bool m_imageCropping = false;
    bool m_imageCropDragging = false;
    QPointF m_imageCropAnchor;
    QRectF m_imageCropRect;
    int m_imagePage = -1;
    QRectF m_imageRect;
    ImageDrag m_imageDrag = ImageDrag::None;
    int m_imageHandle = -1;
    QPointF m_imageGrab;
    QPointF m_imageAnchor;

    // text selection
    bool m_selecting = false;
    bool m_selMoved = false;
    QPoint m_selPressPos;
    QPoint m_lastMouse;
    int m_selStartPage = -1;
    QPointF m_selStartPoint;
    int m_selEndPage = -1;
    QPointF m_selEndPoint;
    QVector<SelectionPart> m_selection;
    QTimer *m_autoScroll = nullptr;
    std::function<bool(int, const QPointF &, FormField *)> m_formFieldHandler;
    std::function<bool(int, int, const QString &)> m_formFieldValueHandler;
    std::function<bool(int, const QPointF &, int *, QRectF *, QString *,
                       TextStyle *)>
        m_textAnnotationHandler;
    std::function<bool(int, const QPointF &, PdfEditor::ImageStamp *)>
        m_imageStampHandler;
    int m_selectedTextAnnotationId = 0;
    int m_selectedTextAnnotationPage = -1;
    QRectF m_selectedTextAnnotationBounds;
    QString m_selectedTextAnnotationContents;
    TextStyle m_selectedTextAnnotationStyle;
    bool m_resizingTextAnnotation = false;
    bool m_movingTextAnnotation = false;
    int m_textAnnotationResizeCorner = -1;
    QPointF m_textAnnotationMoveStart;
    QRectF m_textAnnotationResizeOriginalBounds;
    QString m_selectedImageId;
    int m_selectedImagePage = -1;
    QRectF m_selectedImageBounds;
    QRectF m_imageStampOriginalBounds;
    QImage m_selectedImagePixmap;
    bool m_movingImageStamp = false;
    bool m_resizingImageStamp = false;
    int m_imageStampResizeCorner = -1;
    QPointF m_imageStampMoveStart;
    QVector<FormFieldOverlay> m_formFieldOverlays;
    QWidget *m_formEditor = nullptr;
    QLineEdit *m_formLineEditor = nullptr;
    QPlainTextEdit *m_formTextEditor = nullptr;
    QComboBox *m_formChoiceEditor = nullptr;
    FormField m_activeFormField;
    int m_formEditorPage = -1;
    QString m_formEditorInitialValue;
};