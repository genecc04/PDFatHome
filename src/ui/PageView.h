#pragma once
#include <QAbstractScrollArea>
#include <QCache>
#include <QColor>
#include <QImage>
#include <QList>
#include <QPixmap>
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
#include <functional>

class QPdfDocument;
class QPdfLink;
class QPdfLinkModel;
class QPdfSearchModel;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
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

    void setDocument(QPdfDocument *document);
    void setLoading(bool loading);
    void setPageSizeHint(const QVector<QSizeF> &sizes);
    QVector<QSizeF> pageSizes() const { return m_pageSizes; }
    void rescanPageSizes();
    void stepLoading();
    void setRenderSource(const QString &path, const QString &password = QString());
    void invalidatePageRenders(const QList<int> &changedPages = {});
    void setSearchModel(QPdfSearchModel *model);
    void setCurrentSearchResultIndex(int index);

    int currentPage() const { return m_currentPage; }
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
    void refreshSelectedTextAnnotation();
    void editTextAnnotationInline(int page, int annotationId, const QString &text);
    void deleteSelectedTextAnnotation();
    void setFormFields(int page, const QVector<FormField> &fields);
    void clearFormFieldOverlays();
    bool hasPendingChanges() const;
    void setImageCropMode(bool active);
    void cancelImagePlacement();

    void setDrawToolActive(bool active);
    void setDrawStyle(const DrawStyle &style);
    void setEraserActive(bool active);
    void setInkStrokeHandler(
        const std::function<bool(int, QVector<Stroke> *)> &handler);
    bool hasPendingStrokes() const { return !m_strokes.isEmpty(); }
    bool undoLastStroke();

    void beginImagePlacement(const QImage &image);

    void commitPendingEdits();

    bool hasTextSelection() const { return !m_selection.isEmpty(); }
    void copySelection();
    void selectAllText();
    void clearSelection();

signals:
    void currentPageChanged(int page);
    void textPlaced(int page, const QRectF &boxPoints, const QString &text,
                    const PageView::TextStyle &style);
    void textToolFinished();
    void strokesPlaced(const QVector<PageView::Stroke> &strokes);
    void eraseRequested(int page, const QVector<QPointF> &path, double radius);
    void pendingStrokesChanged();
    void pendingChangesChanged();
    void textAnnotationEditRequested(int page, int annotationId, const QString &text);
    void textAnnotationEdited(int page, int annotationId,
                              const QString &oldText, const QString &newText);
    void textAnnotationDeleteRequested(int page, int annotationId, const QString &text);
    void textAnnotationBoundsChanged(int page, int annotationId, const QString &text,
                                     const QRectF &bounds);
    void textAnnotationSelectionChanged(bool selected);
    void imagePlaced(int page, const QRectF &boxPoints, const QImage &image);
    void imagePlacementStateChanged(bool active, bool positioned);
    void imageCropModeChanged(bool active);
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

    struct SelectionPart {
        int page = -1;
        QList<QPolygonF> bounds;
        QString text;
    };

    struct FormFieldOverlay {
        int page = -1;
        FormField field;
    };

    struct LinkHitRegion {
        QRectF bounds;
        int linkIndex = -1;
    };

    struct PageLinkCache {
        QList<QPdfLink> links;
        QVector<LinkHitRegion> regions;
        QVector<QVector<int>> cells;
        bool loaded = false;
    };

    void onDocumentChanged();
    void invalidatePendingPageRenders();
    void recalculateLayout(bool keepPosition);
    double fitPageZoomFactor() const;
    void positionEmptyStateButton();
    QRect loadingIndicatorRect() const;
    void drawLoadingIndicator(QPainter &painter);
    void updateCurrentPage();
    int pageAtViewportPoint(const QPoint &point) const;
    const QList<QPdfLink> &linksOnPage(int page);
    void loadPageLinks(int page);
    QPdfLink linkAtViewportPoint(const QPoint &point);
    void setHoveredLink(const QUrl &url);
    int nearestPage(const QPoint &viewportPos) const;
    QRect pageRectInViewport(int page) const;
    QPointF pagePointFor(int page, const QPoint &viewportPos) const;
    QPixmap pageImage(int page, const QSize &size, qreal dpr,
                      bool includeAnnotations = true);
    bool updateErasePreview(const QPointF &start, const QPointF &end);
    void drawSearchResults(QPainter &painter, int page, const QRect &pageRect,
                           const QPdfLink &current);
    void drawFormFieldOverlays(QPainter &painter, int page, const QRect &pageRect);
    void drawStroke(QPainter &painter, const Stroke &stroke, const QRect &pageRect);

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
    void extendSelection(const QPoint &viewportPos);
    void selectWordAt(int page, const QPointF &point);
    void selectTextAnnotation(int page, int annotationId, const QRectF &bounds,
                              const QString &text, const TextStyle &style);
    void clearSelectedTextAnnotation();
    QRectF selectedTextAnnotationViewportBounds() const;
    int textAnnotationResizeHandleAt(const QPoint &pos) const;

    QPdfDocument *m_document = nullptr;
    QString m_renderSourcePath;
    QString m_renderPassword;
    quint64 m_renderGeneration = 0;
    QThreadPool m_renderPool;
    QPdfLinkModel *m_linkModel = nullptr;
    QVector<PageLinkCache> m_pageLinkCaches;
    QPdfSearchModel *m_search = nullptr;
    QPushButton *m_emptyStateOpenButton = nullptr;
    QTimer *m_zoomTimer = nullptr;
    QPixmap m_loadingSprite;
    QTimer *m_loadingTimer = nullptr;
    bool m_loading = false;
    int m_loadingFrame = 0;
    int m_searchIndex = -1;

    ZoomMode m_zoomMode = ZoomMode::FitToWidth;
    double m_customZoom = 1.0;
    double m_pendingZoomFactor = 1.0;
    double m_scale = 1.0;
    QVector<QSizeF> m_pageSizes;
    QVector<QSizeF> m_pageSizeHint;
    QVector<QRect> m_pageRects;
    QSize m_documentSize;
    int m_currentPage = 0;
    bool m_jumping = false;
    int m_pressedLinkPage = -1;
    QPoint m_linkPressPos;
    QUrl m_pressedLinkUrl;
    int m_pressedLinkDestinationPage = -1;
    QPointF m_pressedLinkLocation;
    QUrl m_hoveredLinkUrl;

    QCache<quint64, QPixmap> m_cache;
    QCache<quint64, QPixmap> m_baseCache;
    QSet<quint64> m_failedPageKeys;
    QSet<quint64> m_failedBaseKeys;
    QSet<quint64> m_pendingPageKeys;
    QSet<quint64> m_pendingBaseKeys;

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

    // draw tool
    bool m_drawTool = false;
    bool m_drawing = false;
    bool m_eraserActive = false;
    bool m_erasing = false;
    int m_erasePage = -1;
    QPoint m_eraserCursorPos;
    QVector<QPointF> m_erasePoints;
    QVector<ErasePreviewStroke> m_erasePreviewStrokes;
    DrawStyle m_drawStyle;
    std::function<bool(int, QVector<Stroke> *)> m_inkStrokeHandler;
    Stroke m_activeStroke;
    QVector<Stroke> m_strokes;

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
    QVector<FormFieldOverlay> m_formFieldOverlays;
    QWidget *m_formEditor = nullptr;
    QLineEdit *m_formLineEditor = nullptr;
    QPlainTextEdit *m_formTextEditor = nullptr;
    QComboBox *m_formChoiceEditor = nullptr;
    FormField m_activeFormField;
    int m_formEditorPage = -1;
    QString m_formEditorInitialValue;
};