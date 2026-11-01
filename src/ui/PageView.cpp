#include "PageView.h"
#include "MuPdfRenderCache.h"
#include "SearchResultModel.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDebug>
#include <QEventLoop>
#include <QFocusEvent>
#include <QFont>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLineF>
#include <QLoggingCategory>
#include <QMenu>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QPlainTextEdit>
#include <QPolygonF>
#include <QPushButton>
#include <QScrollBar>
#include <QThreadPool>
#include <QStringList>
#include <QTextCursor>
#include <QTimer>
#include <QWheelEvent>
#include <QtMath>
#include <QBuffer>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <memory>

#include <algorithm>
#include <limits>
#include <utility>

namespace {
Q_LOGGING_CATEGORY(lcPageRasterCache, "pdfapp.cache.pages")

constexpr int kMargin = 12;
constexpr int kSpacing = 12;
constexpr int kLinkGridSize = 16;
constexpr int kMinLinksForSpatialIndex = 24;
constexpr int kZoomUpdateIntervalMs = 75;
constexpr double kMinZoom = 0.1;
constexpr double kMaxZoom = 4.0;
constexpr int kMaxPendingPageRenders = 4;
constexpr int kPageImageCacheKilobytes = 64 * 1024;
constexpr int kBasePageImageCacheKilobytes = 16 * 1024;
constexpr int kPageTileCacheKilobytes = 24 * 1024;
constexpr int kSharedPageRasterCacheKilobytes = 64 * 1024;
constexpr int kPageTileSize = 1024;
constexpr int kPageTileThreshold = 4096;
constexpr int kPageCachePrefetchRadius = 2;

constexpr int kLoadingFrameSize = 64;
constexpr int kLoadingDisplaySize = 256;
constexpr int kLoadingFrameIntervalMs = 80;
constexpr int kLoadingCardPadding = 12;
constexpr int kLoadingTopMargin = 16;

class FunctionRunnable final : public QRunnable
{
public:
    explicit FunctionRunnable(std::function<void()> function)
        : m_function(std::move(function))
    {
    }

    void run() override
    {
        m_function();
    }

private:
    std::function<void()> m_function;
};

double pointSegmentDistanceSquared(const QPointF &point, const QPointF &start,
                                   const QPointF &end)
{
    const QPointF segment = end - start;
    const double lengthSquared = QPointF::dotProduct(segment, segment);
    if (lengthSquared == 0.0) {
        const QPointF delta = point - start;
        return QPointF::dotProduct(delta, delta);
    }

    const double projection = qBound(
        0.0, QPointF::dotProduct(point - start, segment) / lengthSquared, 1.0);
    const QPointF delta = point - (start + segment * projection);
    return QPointF::dotProduct(delta, delta);
}

double crossProduct(const QPointF &a, const QPointF &b, const QPointF &c)
{
    return (b.x() - a.x()) * (c.y() - a.y())
           - (b.y() - a.y()) * (c.x() - a.x());
}

bool pointOnSegment(const QPointF &point, const QPointF &start, const QPointF &end)
{
    constexpr double epsilon = 1e-9;
    return qAbs(crossProduct(start, end, point)) <= epsilon
           && point.x() >= qMin(start.x(), end.x()) - epsilon
           && point.x() <= qMax(start.x(), end.x()) + epsilon
           && point.y() >= qMin(start.y(), end.y()) - epsilon
           && point.y() <= qMax(start.y(), end.y()) + epsilon;
}

bool segmentsIntersect(const QPointF &a, const QPointF &b,
                       const QPointF &c, const QPointF &d)
{
    const double abC = crossProduct(a, b, c);
    const double abD = crossProduct(a, b, d);
    const double cdA = crossProduct(c, d, a);
    const double cdB = crossProduct(c, d, b);
    if (((abC > 0.0 && abD < 0.0) || (abC < 0.0 && abD > 0.0))
        && ((cdA > 0.0 && cdB < 0.0) || (cdA < 0.0 && cdB > 0.0)))
        return true;
    return (abC == 0.0 && pointOnSegment(c, a, b))
           || (abD == 0.0 && pointOnSegment(d, a, b))
           || (cdA == 0.0 && pointOnSegment(a, c, d))
           || (cdB == 0.0 && pointOnSegment(b, c, d));
}

double segmentDistanceSquared(const QPointF &a, const QPointF &b,
                              const QPointF &c, const QPointF &d)
{
    if (segmentsIntersect(a, b, c, d))
        return 0.0;

    return qMin(qMin(pointSegmentDistanceSquared(a, c, d),
                     pointSegmentDistanceSquared(b, c, d)),
                qMin(pointSegmentDistanceSquared(c, a, b),
                     pointSegmentDistanceSquared(d, a, b)));
}
}

PageView::PageView(QWidget *parent)
    : QAbstractScrollArea(parent)
    , m_cache(kPageImageCacheKilobytes)
    , m_baseCache(kBasePageImageCacheKilobytes)
    , m_tileCache(kPageTileCacheKilobytes)
{
    m_renderPool.setMaxThreadCount(1);
    setFocusPolicy(Qt::StrongFocus);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    horizontalScrollBar()->setSingleStep(30);
    verticalScrollBar()->setSingleStep(40);
    viewport()->setMouseTracking(true);
    viewport()->installEventFilter(this);
    m_emptyStateOpenButton = new QPushButton("Open a PDF...", viewport());
    m_emptyStateOpenButton->setObjectName(QStringLiteral("emptyStateOpenButton"));
    m_emptyStateOpenButton->setCursor(Qt::PointingHandCursor);
    m_emptyStateOpenButton->setStyleSheet(
        QStringLiteral("QPushButton {"
                       " color: #344050; background: #e9edf2;"
                       " border: 1px solid #d4dae2; border-radius: 5px;"
                       " padding: 9px 22px; font-weight: 600;"
                       " } QPushButton:hover { background: #e1e6ec; }"
                       " QPushButton:pressed { background: #d8dee6; }"));
    connect(m_emptyStateOpenButton, &QPushButton::clicked,
            this, &PageView::openRequested);

    m_zoomTimer = new QTimer(this);
    m_zoomTimer->setSingleShot(true);
    m_zoomTimer->setInterval(kZoomUpdateIntervalMs);
    connect(m_zoomTimer, &QTimer::timeout, this, [this]() {
        setZoomFactor(m_pendingZoomFactor);
    });

    m_loadingSprite.load(QStringLiteral(":/icons/loading64.png"));
    if (m_loadingSprite.isNull()
        || m_loadingSprite.height() != kLoadingFrameSize
        || m_loadingSprite.width() % kLoadingFrameSize != 0) {
        qWarning() << "Loading sprite must be a horizontal strip of 64x64 frames";
        m_loadingSprite = QPixmap();
    }

    m_loadingTimer = new QTimer(this);
    m_loadingTimer->setInterval(kLoadingFrameIntervalMs);
    connect(m_loadingTimer, &QTimer::timeout, this, [this]() {
        const int frames = qMax(1, m_loadingSprite.width() / kLoadingFrameSize);
        m_loadingFrame = (m_loadingFrame + 1) % frames;
        viewport()->update(loadingIndicatorRect());
    });

    m_autoScroll = new QTimer(this);
    m_autoScroll->setInterval(30);
    connect(m_autoScroll, &QTimer::timeout, this, [this]() {
        if (!m_selecting) {
            m_autoScroll->stop();
            return;
        }
        int dy = 0;
        if (m_lastMouse.y() < 0)
            dy = m_lastMouse.y() / 2 - 2;
        else if (m_lastMouse.y() > viewport()->height())
            dy = (m_lastMouse.y() - viewport()->height()) / 2 + 2;
        if (dy != 0) {
            verticalScrollBar()->setValue(verticalScrollBar()->value() + qBound(-40, dy, 40));
            extendSelection(m_lastMouse);
        }
    });
}

PageView::~PageView()
{
    invalidatePendingPageRenders();
    m_renderPool.waitForDone();
}

// document and zoom

void PageView::setDocument(PdfEditor *document, bool preserveRasterPreviews)
{
    m_document = document;
    onDocumentChanged(preserveRasterPreviews);
}

void PageView::setLoading(bool loading)
{
    if (m_loading == loading)
        return;

    m_loading = loading;
    if (loading) {
        finishEditor(false);
        finishFormEditor(false);
        m_autoScroll->stop();
        m_selecting = false;
        m_redactionDrawing = false;
        m_redactionPage = -1;
        m_redactionPreview = QRectF();
        m_drawing = false;
        m_activeStroke = Stroke();
        m_erasing = false;
        m_erasePoints.clear();
        m_erasePreviewStrokes.clear();
        m_pendingCommittedStrokes.clear();
        m_pendingErasedPage = -1;
        m_pendingErasedStrokes.clear();
        m_pendingTextAnnotationPreview = TextAnnotationPreview();
        m_pendingImageStampPreview = ImageStampPreview();
        m_imageDrag = ImageDrag::None;
        m_imageCropDragging = false;
        m_movingTextAnnotation = false;
        m_resizingTextAnnotation = false;
        m_textAnnotationResizeCorner = -1;
        finishImage(false);
    }
    m_loadingFrame = 0;
    if (loading && !m_loadingSprite.isNull())
        m_loadingTimer->start();
    else
        m_loadingTimer->stop();

    positionEmptyStateButton();
    viewport()->update();
}

void PageView::setInteractionBlocked(bool blocked)
{
    m_interactionBlocked = blocked;
}

void PageView::stepLoading()
{
    if (!m_loading || m_loadingSprite.isNull())
        return;

    const int frames = qMax(1, m_loadingSprite.width() / kLoadingFrameSize);
    m_loadingFrame = (m_loadingFrame + 1) % frames;
    viewport()->repaint(loadingIndicatorRect());
}

void PageView::setPageSizeHint(const QVector<QSizeF> &sizes)
{
    m_pageSizeHint = sizes;
}

// Slow (asks the document for every page's size) but always correct. Used as a
// safety net when sizes we worked out ourselves turn out to be wrong.
QRect PageView::loadingIndicatorRect() const
{
    const int side = kLoadingDisplaySize + 2 * kLoadingCardPadding;
    return QRect((viewport()->width() - side) / 2, kLoadingTopMargin, side, side);
}

void PageView::drawLoadingIndicator(QPainter &painter)
{
    if (!m_loading || m_loadingSprite.isNull())
        return;

    const QRect card = loadingIndicatorRect();
    const QRect target(card.x() + kLoadingCardPadding, card.y() + kLoadingCardPadding,
                       kLoadingDisplaySize, kLoadingDisplaySize);
    const QRect source(m_loadingFrame * kLoadingFrameSize, 0,
                       kLoadingFrameSize, kLoadingFrameSize);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, kLoadingDisplaySize < kLoadingFrameSize);
    painter.drawPixmap(target, m_loadingSprite, source);
    painter.restore();
}

void PageView::setRenderSource(const QString &path, const QString &password)
{
    m_renderSourcePath = path;
    m_renderPassword = password;
    invalidatePendingPageRenders();
    m_failedPageKeys.clear();
    m_failedBaseKeys.clear();
    m_failedTileKeys.clear();
    viewport()->update();
}

void PageView::invalidatePageRenders(const QList<int> &changedPages)
{
    if (changedPages.isEmpty()) {
        m_cache.clear();
        m_baseCache.clear();
        m_tileCache.clear();
        m_failedPageKeys.clear();
        m_failedBaseKeys.clear();
        m_failedTileKeys.clear();
        m_stalePageKeys.clear();
        m_staleBaseKeys.clear();
        m_staleTileKeys.clear();
        invalidatePendingPageRenders();
        viewport()->update();
        return;
    }

    invalidatePendingPageRenders();
    const QList<quint64> keys = m_cache.keys();
    for (quint64 key : keys) {
        if (changedPages.contains(int(key >> 40)))
            m_stalePageKeys.insert(key);
    }
    const QList<quint64> baseKeys = m_baseCache.keys();
    for (quint64 key : baseKeys) {
        if (changedPages.contains(int(key >> 40)))
            m_staleBaseKeys.insert(key);
    }
    const QList<QString> tileKeys = m_tileCache.keys();
    for (const QString &key : tileKeys) {
        const int separator = key.indexOf(QLatin1Char(':'));
        bool ok = false;
        const int page = key.left(separator).toInt(&ok);
        if (ok && changedPages.contains(page))
            m_staleTileKeys.insert(key);
    }
    for (int page : changedPages) {
        for (auto it = m_failedPageKeys.begin(); it != m_failedPageKeys.end();) {
            if ((*it >> 40) == quint64(page))
                it = m_failedPageKeys.erase(it);
            else
                ++it;
        }
        for (auto it = m_failedBaseKeys.begin(); it != m_failedBaseKeys.end();) {
            if ((*it >> 40) == quint64(page))
                it = m_failedBaseKeys.erase(it);
            else
                ++it;
        }
        const QString pagePrefix = QString::number(page) + QLatin1Char(':');
        for (auto it = m_failedTileKeys.begin(); it != m_failedTileKeys.end();) {
            if ((*it).startsWith(pagePrefix))
                it = m_failedTileKeys.erase(it);
            else
                ++it;
        }
    }

    viewport()->update();
    const QPoint offset(horizontalScrollBar()->value(), verticalScrollBar()->value());
    for (int page : changedPages) {
        if (page >= 0 && page < m_pageRects.size())
            viewport()->update(m_pageRects.at(page).translated(-offset));
    }
}

void PageView::markCachedRastersStale()
{
    for (quint64 key : m_cache.keys())
        m_stalePageKeys.insert(key);
    for (quint64 key : m_baseCache.keys())
        m_staleBaseKeys.insert(key);
    for (const QString &key : m_tileCache.keys())
        m_staleTileKeys.insert(key);
}

void PageView::onDocumentChanged(bool preserveRasterPreviews)
{
    invalidatePendingPageRenders();
    m_zoomTimer->stop();
    m_pendingZoomFactor = zoomFactor();
    finishEditor(false);
    finishFormEditor(false);
    finishImage(false);
    m_resizingTextAnnotation = false;
    m_movingTextAnnotation = false;
    m_textAnnotationResizeCorner = -1;

    clearSelectedTextAnnotation();
    clearSelectedImageStamp();
    m_selecting = false;
    m_pressedLinkPage = -1;
    m_pressedLinkUrl = QUrl();
    m_pressedLinkDestinationPage = -1;
    m_pressedLinkLocation = QPointF();
    setHoveredLink(QUrl());
    m_selection.clear();
    m_selStartPage = -1;
    m_selEndPage = -1;
    m_erasing = false;
    m_erasePage = -1;
    m_erasePoints.clear();
    m_erasePreviewStrokes.clear();
    m_pendingCommittedStrokes.clear();
    m_pendingErasedPage = -1;
    m_pendingErasedStrokes.clear();
    m_pendingTextAnnotationPreview = TextAnnotationPreview();
    m_pendingImageStampPreview = ImageStampPreview();
    setRedactionToolActive(false);
    clearRedactionAreas();

    const bool hadStrokes = !m_strokes.isEmpty();
    m_strokes.clear();
    m_drawing = false;
    m_activeStroke = Stroke();
    if (hadStrokes)
        emit pendingStrokesChanged();

    if (preserveRasterPreviews) {
        markCachedRastersStale();
    } else {
        m_cache.clear();
        m_baseCache.clear();
        m_tileCache.clear();
        m_stalePageKeys.clear();
        m_staleBaseKeys.clear();
        m_staleTileKeys.clear();
    }
    m_failedPageKeys.clear();
    m_failedBaseKeys.clear();
    m_failedTileKeys.clear();
    m_pageSizes.clear();
    m_pageLinkCaches.clear();
    m_searchIndex = -1;
    m_visibleFirstPage = -1;
    m_visibleLastPage = -1;

    if (m_document) {
        const int count = m_document->pageCount();
        if (count > 0) {
            m_pageLinkCaches.resize(count);
            if (m_pageSizeHint.size() == count) {
                m_pageSizes = m_pageSizeHint;
            } else {
                m_pageSizes = m_document->pageSizes();
            }
        }
    }
    recalculateLayout(false);
}

void PageView::finishPendingRasterPreview(int page, const QSize &tilePageSize)
{
    if (!tilePageSize.isEmpty()) {
        if (page < 0 || page >= m_pageRects.size())
            return;
        const QPoint offset(horizontalScrollBar()->value(), verticalScrollBar()->value());
        const QRect target = m_pageRects.at(page).translated(-offset);
        const QRect visible = target.intersected(viewport()->rect());
        if (visible.isEmpty())
            return;
        const qreal dpr = devicePixelRatioF();
        const int firstTileX = qMax(
            0, int(qFloor((visible.left() - target.left()) * dpr / kPageTileSize)));
        const int lastTileX = qMin(
            (tilePageSize.width() - 1) / kPageTileSize,
            int(qFloor((visible.right() - target.left()) * dpr / kPageTileSize)));
        const int firstTileY = qMax(
            0, int(qFloor((visible.top() - target.top()) * dpr / kPageTileSize)));
        const int lastTileY = qMin(
            (tilePageSize.height() - 1) / kPageTileSize,
            int(qFloor((visible.bottom() - target.top()) * dpr / kPageTileSize)));
        for (int tileY = firstTileY; tileY <= lastTileY; ++tileY) {
            for (int tileX = firstTileX; tileX <= lastTileX; ++tileX) {
                const QString key = QStringLiteral("%1:%2:%3:%4:%5")
                                        .arg(page).arg(tilePageSize.width())
                                        .arg(tilePageSize.height()).arg(tileX).arg(tileY);
                if (!m_tileCache.contains(key) || m_staleTileKeys.contains(key))
                    return;
            }
        }
    }

    m_pendingCommittedStrokes.erase(
        std::remove_if(m_pendingCommittedStrokes.begin(), m_pendingCommittedStrokes.end(),
                       [page](const Stroke &stroke) { return stroke.page == page; }),
        m_pendingCommittedStrokes.end());
    if (m_pendingErasedPage == page) {
        m_pendingErasedPage = -1;
        m_pendingErasedStrokes.clear();
    }
    if (m_pendingTextAnnotationPreview.page == page)
        m_pendingTextAnnotationPreview = TextAnnotationPreview();
    if (m_pendingImageStampPreview.page == page)
        m_pendingImageStampPreview = ImageStampPreview();
    if (page >= 0 && page < m_pageRects.size()) {
        const QPoint offset(horizontalScrollBar()->value(), verticalScrollBar()->value());
        viewport()->update(m_pageRects.at(page).translated(-offset));
    }
}

void PageView::invalidatePendingPageRenders()
{
    discardQueuedPageRenders();
}

void PageView::positionEmptyStateButton()
{
    if (!m_emptyStateOpenButton)
        return;
    const QSize buttonSize = m_emptyStateOpenButton->sizeHint();
    m_emptyStateOpenButton->setGeometry(
        (viewport()->width() - buttonSize.width()) / 2,
        viewport()->height() / 2 + 48,
        buttonSize.width(), buttonSize.height());
    const bool loading = m_loading;
    m_emptyStateOpenButton->setVisible(m_pageRects.isEmpty() && !loading);
}

void PageView::setZoomMode(ZoomMode mode)
{
    invalidatePendingPageRenders();
    m_zoomMode = mode;
    recalculateLayout(true);
}

void PageView::setZoomFactor(double factor)
{
    invalidatePendingPageRenders();
    m_zoomTimer->stop();
    m_customZoom = qBound(fitPageZoomFactor(), factor, kMaxZoom);
    m_zoomMode = ZoomMode::Custom;
    recalculateLayout(true);
}

double PageView::zoomFactor() const
{
    return m_scale / (logicalDpiX() / 72.0);
}

double PageView::fitPageZoomFactor() const
{
    if (m_pageSizes.isEmpty())
        return kMinZoom;

    double maxWidth = 1.0;
    double maxHeight = 1.0;
    for (const QSizeF &size : std::as_const(m_pageSizes)) {
        maxWidth = qMax(maxWidth, size.width());
        maxHeight = qMax(maxHeight, size.height());
    }

    const double fitScale = qMax(
        0.01, qMin((viewport()->width() - 2.0 * kMargin) / maxWidth,
                   (viewport()->height() - 2.0 * kMargin) / maxHeight));
    return qBound(kMinZoom,
                  fitScale / (logicalDpiX() / 72.0), kMaxZoom);
}

void PageView::recalculateLayout(bool keepPosition)
{
    if (m_zoomMode == ZoomMode::Custom)
        m_customZoom = qBound(fitPageZoomFactor(), m_customZoom, kMaxZoom);

    int anchorPage = -1;
    double anchorFraction = 0.0;
    if (keepPosition && !m_pageRects.isEmpty()) {
        anchorPage = qBound(0, m_currentPage, int(m_pageRects.size()) - 1);
        const QRect &r = m_pageRects.at(anchorPage);
        anchorFraction = double(verticalScrollBar()->value() - r.top()) / qMax(1, r.height());
    }

    m_pageRects.clear();
    m_documentSize = QSize();

    if (!m_pageSizes.isEmpty()) {
        double maxWidth = 1.0;
        double maxHeight = 1.0;
        for (const QSizeF &size : std::as_const(m_pageSizes)) {
            maxWidth = qMax(maxWidth, size.width());
            maxHeight = qMax(maxHeight, size.height());
        }

        const int viewWidth = viewport()->width();
        const int viewHeight = viewport()->height();
        switch (m_zoomMode) {
        case ZoomMode::Custom:
            m_scale = m_customZoom * logicalDpiX() / 72.0;
            break;
        case ZoomMode::FitToWidth:
            m_scale = (viewWidth - 2.0 * kMargin) / maxWidth;
            break;
        case ZoomMode::FitInView:
            m_scale = qMin((viewWidth - 2.0 * kMargin) / maxWidth,
                           (viewHeight - 2.0 * kMargin) / maxHeight);
            break;
        }
        m_scale = qBound(0.01, m_scale,
                         kMaxZoom * logicalDpiX() / 72.0);

        QVector<QSize> pixelSizes;
        pixelSizes.reserve(m_pageSizes.size());
        int widest = 0;
        for (const QSizeF &size : std::as_const(m_pageSizes)) {
            const QSize px(qMax(1, qRound(size.width() * m_scale)),
                           qMax(1, qRound(size.height() * m_scale)));
            pixelSizes.append(px);
            widest = qMax(widest, px.width());
        }

        const int contentWidth = qMax(widest + 2 * kMargin, viewWidth);
        int y = kMargin;
        for (const QSize &px : std::as_const(pixelSizes)) {
            m_pageRects.append(QRect((contentWidth - px.width()) / 2, y,
                                     px.width(), px.height()));
            y += px.height() + kSpacing;
        }
        m_documentSize = QSize(contentWidth, y - kSpacing + kMargin);
    }

    horizontalScrollBar()->setRange(0, qMax(0, m_documentSize.width() - viewport()->width()));
    horizontalScrollBar()->setPageStep(viewport()->width());
    verticalScrollBar()->setRange(0, qMax(0, m_documentSize.height() - viewport()->height()));
    verticalScrollBar()->setPageStep(viewport()->height());

    if (anchorPage >= 0 && anchorPage < m_pageRects.size()) {
        const QRect &r = m_pageRects.at(anchorPage);
        verticalScrollBar()->setValue(r.top() + int(anchorFraction * r.height()));
    } else if (!keepPosition) {
        verticalScrollBar()->setValue(0);
        horizontalScrollBar()->setValue(0);
    }

    applyEditorStyle();
    positionEditor();
    positionFormEditor();
    positionEmptyStateButton();
    viewport()->update();
    updateCurrentPage();
    trimPageImageCaches();
}

// navigation

void PageView::updateCurrentPage()
{
    updateVisiblePageRange();
    if (m_pageRects.isEmpty())
        return;

    const int middle = verticalScrollBar()->value() + viewport()->height() / 2;
    const auto nextPage = std::upper_bound(
        m_pageRects.cbegin(), m_pageRects.cend(), middle,
        [](int y, const QRect &rect) { return y < rect.top(); });
    const int page = qMax(0, int(nextPage - m_pageRects.cbegin()) - 1);
    if (page != m_currentPage) {
        m_currentPage = page;
        trimPageImageCaches();
        emit currentPageChanged(page);
    }
}

QPair<int, int> PageView::visiblePageRange() const
{
    if (m_pageRects.isEmpty())
        return qMakePair(-1, -1);

    const int top = verticalScrollBar()->value();
    const int bottom = top + viewport()->height();
    auto first = std::lower_bound(
        m_pageRects.cbegin(), m_pageRects.cend(), top,
        [](const QRect &rect, int y) { return rect.bottom() < y; });
    if (first == m_pageRects.cend() || first->top() > bottom)
        return qMakePair(-1, -1);

    int lastPage = int(first - m_pageRects.cbegin());
    for (auto page = first; page != m_pageRects.cend() && page->top() <= bottom; ++page)
        lastPage = int(page - m_pageRects.cbegin());
    return qMakePair(int(first - m_pageRects.cbegin()), lastPage);
}

void PageView::updateVisiblePageRange()
{
    const QPair<int, int> range = visiblePageRange();
    if (range.first == m_visibleFirstPage && range.second == m_visibleLastPage)
        return;

    m_visibleFirstPage = range.first;
    m_visibleLastPage = range.second;
    emit visiblePageRangeChanged(range.first, range.second);
}

void PageView::jumpToPage(int page, const QPointF &location)
{
    if (page < 0 || page >= m_pageRects.size())
        return;
    const QRect &r = m_pageRects.at(page);

    m_jumping = true;
    if (location.isNull()) {
        verticalScrollBar()->setValue(r.top() - kMargin);
    } else {
        verticalScrollBar()->setValue(
            r.top() + int(location.y() * m_scale) - viewport()->height() / 3);
        horizontalScrollBar()->setValue(
            r.left() + int(location.x() * m_scale) - viewport()->width() / 2);
    }
    m_jumping = false;

    if (page != m_currentPage) {
        m_currentPage = page;
        trimPageImageCaches();
        emit currentPageChanged(page);
    }
    viewport()->update();
    updateVisiblePageRange();
}

void PageView::trimPageImageCaches()
{
    if (m_pageRects.isEmpty() || viewport()->size().isEmpty())
        return;

    const QPoint offset(horizontalScrollBar()->value(), verticalScrollBar()->value());
    const int visibleTop = offset.y();
    const int visibleBottom = visibleTop + viewport()->height();
    const auto firstVisible = std::lower_bound(
        m_pageRects.cbegin(), m_pageRects.cend(), visibleTop,
        [](const QRect &rect, int y) { return rect.bottom() < y; });
    if (firstVisible == m_pageRects.cend())
        return;

    const int first = int(firstVisible - m_pageRects.cbegin());
    int last = first;
    while (last + 1 < m_pageRects.size()
           && m_pageRects.at(last + 1).top() <= visibleBottom) {
        ++last;
    }

    const int keepFirst = qMax(0, first - kPageCachePrefetchRadius);
    const int keepLast = qMin(m_pageRects.size() - 1, last + kPageCachePrefetchRadius);
    int evicted = 0;
    int evictedLinkPages = 0;
    for (int page = 0; page < m_pageLinkCaches.size(); ++page) {
        if ((page < keepFirst || page > keepLast)
            && m_pageLinkCaches.at(page).loaded) {
            m_pageLinkCaches[page] = PageLinkCache();
            ++evictedLinkPages;
        }
    }

    const auto evictDistantPages = [&](QCache<quint64, QPixmap> &cache,
                                       QSet<quint64> &staleKeys) {
        const QList<quint64> keys = cache.keys();
        for (quint64 key : keys) {
            const int page = int(key >> 40);
            if (page < keepFirst || page > keepLast) {
                cache.remove(key);
                staleKeys.remove(key);
                ++evicted;
            }
        }
    };
    evictDistantPages(m_cache, m_stalePageKeys);
    evictDistantPages(m_baseCache, m_staleBaseKeys);
    const QList<QString> tileKeys = m_tileCache.keys();
    for (const QString &key : tileKeys) {
        const int separator = key.indexOf(QLatin1Char(':'));
        bool ok = false;
        const int page = key.left(separator).toInt(&ok);
        if (ok && (page < keepFirst || page > keepLast)) {
            m_tileCache.remove(key);
            m_staleTileKeys.remove(key);
            ++evicted;
        }
    }

    for (auto it = m_failedPageKeys.begin(); it != m_failedPageKeys.end();) {
        const int page = int(*it >> 40);
        if (page < keepFirst || page > keepLast)
            it = m_failedPageKeys.erase(it);
        else
            ++it;
    }
    for (auto it = m_failedBaseKeys.begin(); it != m_failedBaseKeys.end();) {
        const int page = int(*it >> 40);
        if (page < keepFirst || page > keepLast)
            it = m_failedBaseKeys.erase(it);
        else
            ++it;
    }
    for (auto it = m_failedTileKeys.begin(); it != m_failedTileKeys.end();) {
        const int separator = it->indexOf(QLatin1Char(':'));
        bool ok = false;
        const int page = it->left(separator).toInt(&ok);
        if (ok && (page < keepFirst || page > keepLast))
            it = m_failedTileKeys.erase(it);
        else
            ++it;
    }

    if (evicted > 0) {
        qCDebug(lcPageRasterCache)
            << "Evicted distant page rasters:" << evicted
            << "visible pages:" << first + 1 << "-" << last + 1
            << "retained range:" << keepFirst + 1 << "-" << keepLast + 1
            << "cache bytes:"
            << qint64(m_cache.totalCost() + m_baseCache.totalCost()
                      + m_tileCache.totalCost()) * 1024;
    }
    if (evictedLinkPages > 0)
        qCDebug(lcPageRasterCache) << "Evicted link data for distant pages:"
                                   << evictedLinkPages;
}

void PageView::discardQueuedPageRenders()
{
    ++m_renderGeneration;
    m_renderPool.clear();
    m_pendingPageKeys.clear();
    m_pendingBaseKeys.clear();
    m_pendingTileKeys.clear();
    m_renderRequestsSaturated = false;
}

void PageView::enforcePageRasterBudget()
{
    const auto totalCost = [this]() {
        return m_cache.totalCost() + m_baseCache.totalCost()
               + m_tileCache.totalCost();
    };
    const auto visibleRange = [this]() {
        QPair<int, int> range(m_currentPage, m_currentPage);
        if (m_pageRects.isEmpty() || viewport()->height() <= 0)
            return range;
        const int top = verticalScrollBar()->value();
        const int bottom = top + viewport()->height();
        const auto first = std::lower_bound(
            m_pageRects.cbegin(), m_pageRects.cend(), top,
            [](const QRect &rect, int y) { return rect.bottom() < y; });
        if (first == m_pageRects.cend())
            return range;
        range.first = int(first - m_pageRects.cbegin());
        range.second = range.first;
        while (range.second + 1 < m_pageRects.size()
               && m_pageRects.at(range.second + 1).top() <= bottom) {
            ++range.second;
        }
        return range;
    }();
    const auto distanceFromVisible = [&visibleRange](int page) {
        if (page < visibleRange.first)
            return visibleRange.first - page;
        if (page > visibleRange.second)
            return page - visibleRange.second;
        return 0;
    };

    int evicted = 0;
    while (totalCost() > kSharedPageRasterCacheKilobytes) {
        int farthestPage = -1;
        int farthestDistance = -1;
        const auto considerPage = [&](int page) {
            const int distance = distanceFromVisible(page);
            if (distance > farthestDistance) {
                farthestDistance = distance;
                farthestPage = page;
            }
        };
        for (quint64 key : m_baseCache.keys())
            considerPage(int(key >> 40));
        for (quint64 key : m_cache.keys())
            considerPage(int(key >> 40));
        for (const QString &key : m_tileCache.keys()) {
            const int separator = key.indexOf(QLatin1Char(':'));
            bool ok = false;
            const int page = key.left(separator).toInt(&ok);
            if (ok)
                considerPage(page);
        }
        if (farthestPage < 0)
            break;

        bool removed = false;
        const auto removeOneForPage = [farthestPage](
                                          QCache<quint64, QPixmap> &cache,
                                          QSet<quint64> &staleKeys) {
            for (quint64 key : cache.keys()) {
                if (int(key >> 40) == farthestPage) {
                    cache.remove(key);
                    staleKeys.remove(key);
                    return true;
                }
            }
            return false;
        };
        removed = removeOneForPage(m_baseCache, m_staleBaseKeys);
        if (!removed)
            removed = removeOneForPage(m_cache, m_stalePageKeys);
        if (!removed) {
            for (const QString &key : m_tileCache.keys()) {
                const int separator = key.indexOf(QLatin1Char(':'));
                bool ok = false;
                const int page = key.left(separator).toInt(&ok);
                if (ok && page == farthestPage) {
                    m_tileCache.remove(key);
                    m_staleTileKeys.remove(key);
                    removed = true;
                    break;
                }
            }
        }
        if (!removed)
            break;
        ++evicted;
    }

    if (evicted > 0) {
        qCDebug(lcPageRasterCache)
            << "Shared raster budget evicted:" << evicted
            << "cache bytes:" << qint64(totalCost()) * 1024
            << "budget bytes:" << qint64(kSharedPageRasterCacheKilobytes) * 1024;
    }
}

int PageView::pageAtViewportPoint(const QPoint &point) const
{
    const QPoint doc = point + QPoint(horizontalScrollBar()->value(),
                                      verticalScrollBar()->value());
    const auto page = std::lower_bound(
        m_pageRects.cbegin(), m_pageRects.cend(), doc.y(),
        [](const QRect &rect, int y) { return rect.bottom() < y; });
    if (page == m_pageRects.cend() || !page->contains(doc))
        return -1;
    return int(page - m_pageRects.cbegin());
}

const QVector<PdfEditor::Link> &PageView::linksOnPage(int page)
{
    static const QVector<PdfEditor::Link> emptyLinks;
    if (!m_document || page < 0 || page >= m_pageLinkCaches.size())
        return emptyLinks;
    if (!m_pageLinkCaches.at(page).loaded)
        loadPageLinks(page);
    return m_pageLinkCaches.at(page).links;
}

void PageView::loadPageLinks(int page)
{
    PageLinkCache &cache = m_pageLinkCaches[page];
    cache.links = m_document->linksOnPage(page);
    if (!m_document->lastError().isEmpty()) {
        qWarning().noquote() << "Could not load links for page" << page + 1
                             << ":" << m_document->lastError();
    }
    for (int row = 0; row < cache.links.size(); ++row) {
        LinkHitRegion region;
        region.bounds = cache.links.at(row).bounds;
        region.linkIndex = row;
        cache.regions.append(region);
    }

    if (cache.regions.size() >= kMinLinksForSpatialIndex) {
        cache.cells.resize(kLinkGridSize * kLinkGridSize);
        const QSizeF pageSize = m_pageSizes.at(page);
        const auto cellFor = [](qreal coordinate, qreal extent) {
            return qBound(0, int(coordinate * kLinkGridSize / extent),
                          kLinkGridSize - 1);
        };
        for (int regionIndex = 0; regionIndex < cache.regions.size(); ++regionIndex) {
            const QRectF &bounds = cache.regions.at(regionIndex).bounds;
            const int left = cellFor(bounds.left(), pageSize.width());
            const int right = cellFor(bounds.right(), pageSize.width());
            const int top = cellFor(bounds.top(), pageSize.height());
            const int bottom = cellFor(bounds.bottom(), pageSize.height());
            for (int y = top; y <= bottom; ++y) {
                for (int x = left; x <= right; ++x)
                    cache.cells[y * kLinkGridSize + x].append(regionIndex);
            }
        }
    }
    cache.loaded = true;
}

PdfEditor::Link PageView::linkAtViewportPoint(const QPoint &point)
{
    const int page = pageAtViewportPoint(point);
    if (page < 0)
        return {};

    const QPointF pagePoint = pagePointFor(page, point);
    const QVector<PdfEditor::Link> &links = linksOnPage(page);
    const PageLinkCache &cache = m_pageLinkCaches.at(page);
    if (!cache.cells.isEmpty()) {
        const QSizeF pageSize = m_pageSizes.at(page);
        const auto cellFor = [](qreal coordinate, qreal extent) {
            return qBound(0, int(coordinate * kLinkGridSize / extent),
                          kLinkGridSize - 1);
        };
        const int x = cellFor(pagePoint.x(), pageSize.width());
        const int y = cellFor(pagePoint.y(), pageSize.height());
        for (int regionIndex : cache.cells.at(y * kLinkGridSize + x)) {
            const LinkHitRegion &region = cache.regions.at(regionIndex);
            if (region.bounds.contains(pagePoint))
                return links.at(region.linkIndex);
        }
        return {};
    }

    for (const LinkHitRegion &region : cache.regions) {
        if (region.bounds.contains(pagePoint))
            return links.at(region.linkIndex);
    }
    return {};
}

void PageView::setHoveredLink(const QUrl &url)
{
    const QString scheme = url.scheme().toLower();
    const QUrl websiteUrl = scheme == QLatin1String("http")
                                || scheme == QLatin1String("https")
        ? url : QUrl();
    if (websiteUrl == m_hoveredLinkUrl)
        return;

    m_hoveredLinkUrl = websiteUrl;
    emit linkHovered(websiteUrl);
}

int PageView::nearestPage(const QPoint &viewportPos) const
{
    const int y = viewportPos.y() + verticalScrollBar()->value();
    const auto page = std::lower_bound(
        m_pageRects.cbegin(), m_pageRects.cend(), y,
        [](const QRect &rect, int position) { return rect.bottom() < position; });
    if (page == m_pageRects.cend())
        return qMax(0, int(m_pageRects.size()) - 1);
    return int(page - m_pageRects.cbegin());
}

QRect PageView::pageRectInViewport(int page) const
{
    return m_pageRects.at(page).translated(-horizontalScrollBar()->value(),
                                           -verticalScrollBar()->value());
}

QPointF PageView::pagePointFor(int page, const QPoint &viewportPos) const
{
    const QRect r = pageRectInViewport(page);
    const QSizeF size = m_pageSizes.at(page);
    return QPointF(qBound(0.0, (viewportPos.x() - r.x()) / m_scale, size.width()),
                   qBound(0.0, (viewportPos.y() - r.y()) / m_scale, size.height()));
}

// search results

void PageView::setSearchModel(SearchResultModel *model)
{
    m_search = model;
    if (!m_search)
        return;
    auto repaint = [this]() { viewport()->update(); };
    connect(m_search, &QAbstractItemModel::modelReset, this, repaint);
    connect(m_search, &QAbstractItemModel::rowsInserted, this, repaint);
}

void PageView::setCurrentSearchResultIndex(int index)
{
    m_searchIndex = index;
    viewport()->update();
}

void PageView::drawSearchResults(QPainter &painter, int page, const QRect &pageRect,
                                 const PdfEditor::SearchResult &current)
{
    if (!m_search)
        return;
    const QVector<PdfEditor::SearchResult> &results = m_search->resultsOnPage(page);
    for (const PdfEditor::SearchResult &result : results) {
        const bool isCurrent = current.page == page && current.location == result.location;
        for (const QRectF &r : result.rectangles) {
            const QRectF px(pageRect.x() + r.x() * m_scale, pageRect.y() + r.y() * m_scale,
                            r.width() * m_scale, r.height() * m_scale);
            painter.fillRect(px, isCurrent ? QColor(255, 140, 0, 110) : QColor(255, 235, 0, 100));
            if (isCurrent) {
                painter.setPen(QPen(QColor(255, 100, 0), 2));
                painter.drawRect(px);
            }
        }
    }
}

void PageView::drawFormFieldOverlays(QPainter &painter, int page, const QRect &pageRect)
{
    for (const FormFieldOverlay &overlay : std::as_const(m_formFieldOverlays)) {
        if (overlay.page != page)
            continue;

        const QRectF bounds(
            pageRect.x() + overlay.field.bounds.x() * m_scale,
            pageRect.y() + overlay.field.bounds.y() * m_scale,
            overlay.field.bounds.width() * m_scale,
            overlay.field.bounds.height() * m_scale);
        painter.save();
        painter.setClipRect(pageRect, Qt::IntersectClip);
        painter.fillRect(bounds, Qt::white);
        painter.setPen(QPen(QColor(130, 130, 130), 1));

        QString text = overlay.field.value;
        if (overlay.field.password)
            text = QString(text.size(), QLatin1Char('*'));
        if (overlay.field.type == FormField::Type::ComboBox
            || overlay.field.type == FormField::Type::ListBox) {
            const int valueIndex = overlay.field.optionValues.indexOf(text);
            if (valueIndex >= 0 && valueIndex < overlay.field.options.size())
                text = overlay.field.options.at(valueIndex);
        }

        if (overlay.field.type == FormField::Type::CheckBox) {
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(bounds.adjusted(0.5, 0.5, -0.5, -0.5));
            if (overlay.field.checked) {
                painter.setPen(QPen(Qt::black, qMax(1.0, bounds.width() * 0.09),
                                    Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter.drawLine(bounds.left() + bounds.width() * 0.22,
                                 bounds.center().y(),
                                 bounds.left() + bounds.width() * 0.43,
                                 bounds.bottom() - bounds.height() * 0.24);
                painter.drawLine(bounds.left() + bounds.width() * 0.43,
                                 bounds.bottom() - bounds.height() * 0.24,
                                 bounds.right() - bounds.width() * 0.16,
                                 bounds.top() + bounds.height() * 0.2);
            }
            painter.restore();
            continue;
        }
        if (overlay.field.type == FormField::Type::RadioButton) {
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(bounds.adjusted(0.5, 0.5, -0.5, -0.5));
            if (overlay.field.checked) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(Qt::black);
                painter.drawEllipse(bounds.adjusted(bounds.width() * 0.27,
                                                    bounds.height() * 0.27,
                                                    -bounds.width() * 0.27,
                                                    -bounds.height() * 0.27));
            }
            painter.restore();
            continue;
        }

        painter.setBrush(Qt::NoBrush);
        painter.drawRect(bounds.adjusted(0.5, 0.5, -0.5, -0.5));
        const qreal fontSize = qBound(6.0, bounds.height() * 0.58, 18.0);
        QFont font = painter.font();
        font.setPixelSize(qMax(1, qRound(fontSize)));
        painter.setFont(font);
        painter.setPen(Qt::black);
        const qreal arrowWidth = qMin(18.0, qMax(12.0, bounds.height() * 0.9));
        const qreal rightPadding = overlay.field.type == FormField::Type::ComboBox
                                       ? arrowWidth + 3 : 3;
        const QRectF textBounds = bounds.adjusted(3, 1, -rightPadding, -1);
        if (overlay.field.multiline) {
            painter.drawText(textBounds, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, text);
        } else {
            painter.drawText(textBounds, Qt::AlignLeft | Qt::AlignVCenter, text);
        }
        if (overlay.field.type == FormField::Type::ComboBox) {
            const QRectF buttonBounds(bounds.right() - arrowWidth, bounds.top() + 1,
                                      arrowWidth - 1, qMax(0.0, bounds.height() - 2));
            painter.fillRect(buttonBounds, QColor(240, 240, 240));
            painter.setPen(QColor(150, 150, 150));
            painter.drawLine(QPointF(buttonBounds.left(), bounds.top() + 1),
                             QPointF(buttonBounds.left(), bounds.bottom() - 1));

            const QPointF center = buttonBounds.center();
            const qreal halfArrowWidth = qMin(4.0, buttonBounds.width() * 0.28);
            const qreal halfArrowHeight = qMin(2.5, buttonBounds.height() * 0.16);
            QPolygonF arrow;
            arrow << QPointF(center.x() - halfArrowWidth, center.y() - halfArrowHeight)
                  << QPointF(center.x() + halfArrowWidth, center.y() - halfArrowHeight)
                  << QPointF(center.x(), center.y() + halfArrowHeight);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(55, 55, 55));
            painter.drawPolygon(arrow);
        }
        painter.restore();
    }
}

// painting

QPixmap PageView::pageImage(int page, const QSize &size, qreal dpr,
                            bool includeAnnotations)
{
    const quint64 key = (quint64(page) << 40) | (quint64(size.width()) << 20)
                        | quint64(size.height());
    QCache<quint64, QPixmap> &cache = includeAnnotations ? m_cache : m_baseCache;
    QSet<quint64> &failedKeys = includeAnnotations
                                    ? m_failedPageKeys : m_failedBaseKeys;
    QSet<quint64> &pendingKeys = includeAnnotations
                                     ? m_pendingPageKeys : m_pendingBaseKeys;
    QPixmap *cached = cache.object(key);
    const bool stale = includeAnnotations ? m_stalePageKeys.contains(key)
                                          : m_staleBaseKeys.contains(key);
    if (cached && !stale)
        return *cached;
    if (failedKeys.contains(key))
        return cached ? *cached : QPixmap();

    if (!pendingKeys.contains(key)
        && m_pendingPageKeys.size() + m_pendingBaseKeys.size()
                   + m_pendingTileKeys.size() < kMaxPendingPageRenders) {
        pendingKeys.insert(key);
        const quint64 generation = m_renderGeneration;
        const QString sourcePath = m_renderSourcePath;
        const QString password = m_renderPassword;
        const QSize renderSize = size * dpr;
        const int priority = -qAbs(page - m_currentPage);
        m_renderPool.start(new FunctionRunnable(
            [this, page, key, generation, sourcePath, password, renderSize,
             dpr, includeAnnotations]() {
                thread_local MuPdfRenderCache renderer;

                QImage image;
                QString error;
                if (renderer.open(sourcePath, password, &error)) {
                    image = renderer.render(page, renderSize, includeAnnotations, &error);
                    if (image.isNull() && error.isEmpty())
                        error = QStringLiteral("PDF renderer returned an empty page image.");
                }
                QMetaObject::invokeMethod(
                    this,
                    [this, page, key, generation, dpr, includeAnnotations,
                     image = std::move(image), error = std::move(error)]() {
                        if (generation != m_renderGeneration)
                            return;

                        QSet<quint64> &pending = includeAnnotations
                                                     ? m_pendingPageKeys : m_pendingBaseKeys;
                        QSet<quint64> &failed = includeAnnotations
                                                    ? m_failedPageKeys : m_failedBaseKeys;
                        QCache<quint64, QPixmap> &targetCache = includeAnnotations
                                                                   ? m_cache : m_baseCache;
                        pending.remove(key);

                        if (image.isNull()) {
                            failed.insert(key);
                            qWarning().noquote()
                                << QStringLiteral("Could not render PDF page %1: %2")
                                       .arg(page + 1).arg(error);
                        } else {
                            failed.remove(key);
                            if (includeAnnotations)
                                m_stalePageKeys.remove(key);
                            else
                                m_staleBaseKeys.remove(key);
                            QPixmap pixmap = QPixmap::fromImage(image);
                            pixmap.setDevicePixelRatio(dpr);
                            const qint64 bytes =
                                qint64(pixmap.width()) * pixmap.height() * 4;
                            const int costKilobytes = int(qBound<qint64>(
                                qint64(1), (bytes + 1023) / 1024,
                                qint64(std::numeric_limits<int>::max())));
                            targetCache.insert(
                                key, new QPixmap(pixmap), costKilobytes);
                            enforcePageRasterBudget();
                            qCDebug(lcPageRasterCache)
                                << "Raster cache bytes:"
                                << qint64(m_cache.totalCost() + m_baseCache.totalCost()
                                          + m_tileCache.totalCost()) * 1024
                                << "images:" << m_cache.size() + m_baseCache.size()
                                                   + m_tileCache.size();
                            if (includeAnnotations)
                                finishPendingRasterPreview(page);
                        }
                        trimPageImageCaches();

                        if (page >= 0 && page < m_pageRects.size()) {
                            const QPoint offset(horizontalScrollBar()->value(),
                                                verticalScrollBar()->value());
                            viewport()->update(
                                m_pageRects.at(page).translated(-offset));
                        }
                        if (m_renderRequestsSaturated) {
                            m_renderRequestsSaturated = false;
                            viewport()->update();
                        }
                    },
                    Qt::QueuedConnection);
            }), priority);
    } else if (!pendingKeys.contains(key)) {
        m_renderRequestsSaturated = true;
    }

    if (cached)
        return *cached;

    const QList<quint64> candidates = cache.keys();
    QPixmap closestPreview;
    double closestScore = std::numeric_limits<double>::max();
    for (quint64 candidateKey : candidates) {
        if ((candidateKey >> 40) != quint64(page))
            continue;
        QPixmap *candidate = cache.object(candidateKey);
        if (!candidate)
            continue;
        const double logicalWidth = candidate->width() / candidate->devicePixelRatio();
        const double logicalHeight = candidate->height() / candidate->devicePixelRatio();
        const double score = qAbs(qLn(qMax(1.0, logicalWidth) / qMax(1, size.width())))
                             + qAbs(qLn(qMax(1.0, logicalHeight) / qMax(1, size.height())));
        if (score < closestScore) {
            closestScore = score;
            closestPreview = *candidate;
        }
    }
    return closestPreview;
}

QPixmap PageView::pageTile(int page, const QSize &fullSize, int tileX, int tileY,
                           int tileSize, qreal dpr, bool includeAnnotations)
{
    const int pixelX = tileX * tileSize;
    const int pixelY = tileY * tileSize;
    const QSize tilePixels(qMin(tileSize, fullSize.width() - pixelX),
                           qMin(tileSize, fullSize.height() - pixelY));
    if (tilePixels.isEmpty() || page < 0 || page >= m_pageSizes.size())
        return {};

    const QString key = QStringLiteral("%1:%2:%3:%4:%5")
                            .arg(page).arg(fullSize.width()).arg(fullSize.height())
                            .arg(tileX).arg(tileY);
    QPixmap *cached = m_tileCache.object(key);
    const bool stale = m_staleTileKeys.contains(key);
    if (cached && !stale)
        return *cached;
    if (m_failedTileKeys.contains(key))
        return cached ? *cached : QPixmap();

    if (!m_pendingTileKeys.contains(key)
        && m_pendingPageKeys.size() + m_pendingBaseKeys.size()
                   + m_pendingTileKeys.size() < kMaxPendingPageRenders) {
        m_pendingTileKeys.insert(key);
        const quint64 generation = m_renderGeneration;
        const QString sourcePath = m_renderSourcePath;
        const QString password = m_renderPassword;
        const QSizeF pageSize = m_pageSizes.at(page);
        const QRectF pageRegion(
            pageSize.width() * pixelX / fullSize.width(),
            pageSize.height() * pixelY / fullSize.height(),
            pageSize.width() * tilePixels.width() / fullSize.width(),
            pageSize.height() * tilePixels.height() / fullSize.height());

        m_renderPool.start(QRunnable::create(
            [this, page, key, generation, sourcePath, password, pageRegion,
             fullSize, tilePixels, dpr, includeAnnotations]() {
                thread_local MuPdfRenderCache renderer;
                QImage image;
                QString error;
                if (renderer.open(sourcePath, password, &error)) {
                    image = renderer.renderRegion(page, pageRegion, tilePixels,
                                                  includeAnnotations, &error);
                    if (image.isNull() && error.isEmpty())
                        error = QStringLiteral("PDF renderer returned an empty page tile.");
                }
                QMetaObject::invokeMethod(
                    this,
                    [this, page, key, generation, dpr, fullSize, includeAnnotations,
                     image = std::move(image),
                     error = std::move(error)]() {
                        if (generation != m_renderGeneration)
                            return;
                        m_pendingTileKeys.remove(key);
                        if (image.isNull()) {
                            m_failedTileKeys.insert(key);
                            qWarning().noquote()
                                << QStringLiteral("Could not render tile for PDF page %1: %2")
                                       .arg(page + 1).arg(error);
                        } else {
                            m_staleTileKeys.remove(key);
                            QPixmap pixmap = QPixmap::fromImage(image);
                            pixmap.setDevicePixelRatio(dpr);
                            const qint64 bytes =
                                qint64(pixmap.width()) * pixmap.height() * 4;
                            const int costKilobytes = int(qBound<qint64>(
                                qint64(1), (bytes + 1023) / 1024,
                                qint64(std::numeric_limits<int>::max())));
                            m_tileCache.insert(key, new QPixmap(pixmap), costKilobytes);
                            enforcePageRasterBudget();
                            qCDebug(lcPageRasterCache)
                                << "Raster cache bytes:"
                                << qint64(m_cache.totalCost() + m_baseCache.totalCost()
                                          + m_tileCache.totalCost()) * 1024
                                << "images:" << m_cache.size() + m_baseCache.size()
                                                   + m_tileCache.size();
                            if (includeAnnotations)
                                finishPendingRasterPreview(page, fullSize);
                        }
                        trimPageImageCaches();
                        if (page >= 0 && page < m_pageRects.size()) {
                            const QPoint offset(horizontalScrollBar()->value(),
                                                verticalScrollBar()->value());
                            viewport()->update(
                                m_pageRects.at(page).translated(-offset));
                        }
                        if (m_renderRequestsSaturated) {
                            m_renderRequestsSaturated = false;
                            viewport()->update();
                        }
                    },
                    Qt::QueuedConnection);
            }), -qAbs(page - m_currentPage));
    } else if (!m_pendingTileKeys.contains(key)) {
        m_renderRequestsSaturated = true;
    }

    return cached ? *cached : QPixmap();
}

void PageView::drawTiledPage(QPainter &painter, int page, const QRect &target,
                             const QRect &exposed, qreal dpr)
{
    if (page < 0 || page >= m_pageSizes.size() || dpr <= 0.0)
        return;
    const QSize fullSize(qMax(1, qRound(target.width() * dpr)),
                         qMax(1, qRound(target.height() * dpr)));
    const QRect visible = target.intersected(exposed);
    if (visible.isEmpty())
        return;

    const int firstTileX = qMax(
        0, int(qFloor((visible.left() - target.left()) * dpr / kPageTileSize)));
    const int lastTileX = qMin(
        (fullSize.width() - 1) / kPageTileSize,
        int(qFloor((visible.right() - target.left()) * dpr / kPageTileSize)));
    const int firstTileY = qMax(
        0, int(qFloor((visible.top() - target.top()) * dpr / kPageTileSize)));
    const int lastTileY = qMin(
        (fullSize.height() - 1) / kPageTileSize,
        int(qFloor((visible.bottom() - target.top()) * dpr / kPageTileSize)));

    for (int tileY = firstTileY; tileY <= lastTileY; ++tileY) {
        for (int tileX = firstTileX; tileX <= lastTileX; ++tileX) {
            const QPixmap tile = pageTile(page, fullSize, tileX, tileY,
                                          kPageTileSize, dpr, true);
            if (tile.isNull())
                continue;
            const int pixelX = tileX * kPageTileSize;
            const int pixelY = tileY * kPageTileSize;
            const QRectF destination(
                target.left() + pixelX / dpr,
                target.top() + pixelY / dpr,
                tile.width() / dpr,
                tile.height() / dpr);
            painter.drawPixmap(destination, tile, tile.rect());
        }
    }
}

void PageView::drawStroke(QPainter &painter, const Stroke &stroke, const QRect &pageRect)
{
    if (stroke.points.isEmpty())
        return;

    QPolygonF polygon;
    for (const QPointF &p : stroke.points)
        polygon << QPointF(pageRect.x() + p.x() * m_scale, pageRect.y() + p.y() * m_scale);

    QColor color = stroke.style.color;
    color.setAlphaF(stroke.style.opacity);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(color, qMax(1.0, stroke.style.widthPoints * m_scale),
                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    if (polygon.size() == 1)
        painter.drawPoint(polygon.first());
    else
        painter.drawPolyline(polygon);
    painter.restore();
}

void PageView::paintEvent(QPaintEvent *event)
{
    QPainter painter(viewport());
    const bool loading = m_loading;
    if (m_pageRects.isEmpty()) {
        painter.fillRect(event->rect(), QColor(245, 247, 250));
        if (!loading) {
            painter.setPen(QColor(42, 52, 64));
            QFont titleFont = painter.font();
            titleFont.setPixelSize(22);
            titleFont.setWeight(QFont::DemiBold);
            painter.setFont(titleFont);
            const QRect titleRect(0, viewport()->height() / 2 - 66,
                                  viewport()->width(), 34);
            painter.drawText(titleRect, Qt::AlignHCenter | Qt::AlignVCenter,
                             QStringLiteral("No PDF open"));

            QFont hintFont = painter.font();
            hintFont.setPixelSize(14);
            hintFont.setWeight(QFont::Normal);
            painter.setFont(hintFont);
            painter.setPen(QColor(100, 110, 122));
            const QRect hintRect(16, viewport()->height() / 2 - 27,
                                 viewport()->width() - 32, 28);
            painter.drawText(hintRect, Qt::AlignHCenter | Qt::AlignVCenter,
                             QStringLiteral("Drag a PDF here or open one to get started"));
        }
        drawLoadingIndicator(painter);
        return;
    }
    painter.fillRect(event->rect(), QColor(70, 70, 70));
    if (!m_document)
        return;

    const QPoint offset(horizontalScrollBar()->value(), verticalScrollBar()->value());
    const QRect visible(offset, viewport()->size());
    const qreal dpr = devicePixelRatioF();

    PdfEditor::SearchResult current;
    if (m_search && m_searchIndex >= 0
        && m_searchIndex < m_search->rowCount(QModelIndex())) {
        current = m_search->resultAtIndex(m_searchIndex);
    }

    const auto firstVisiblePage = std::lower_bound(
        m_pageRects.cbegin(), m_pageRects.cend(), visible.top(),
        [](const QRect &rect, int y) { return rect.bottom() < y; });
    for (int page = int(firstVisiblePage - m_pageRects.cbegin());
         page < m_pageRects.size(); ++page) {
        const QRect &r = m_pageRects.at(page);
        if (r.top() > visible.bottom())
            break;

        const QRect target = r.translated(-offset);
        painter.fillRect(target, Qt::white);
        const QSize fullRasterSize(qMax(1, qRound(target.width() * dpr)),
                                   qMax(1, qRound(target.height() * dpr)));
        const bool tiledPage =
            (fullRasterSize.width() > kPageTileThreshold
             || fullRasterSize.height() > kPageTileThreshold)
            && !(m_erasing && m_erasePage == page)
            && m_pendingErasedPage != page;
        QPixmap renderedPage;
        if (tiledPage)
            drawTiledPage(painter, page, target, event->rect(), dpr);
        else
            renderedPage = pageImage(page, target.size(), dpr);
        const bool exactPageImage = !renderedPage.isNull()
                                    && qAbs(renderedPage.width() / dpr
                                            - target.width()) < 1.0
                                    && qAbs(renderedPage.height() / dpr
                                            - target.height()) < 1.0;
        QPainterPath erasedInk;
        const auto addErasedStrokes = [this, &erasedInk](const QVector<Stroke> &strokes) {
            for (const Stroke &stroke : strokes) {
                if (stroke.points.isEmpty())
                    continue;
                QPainterPath strokePath;
                const QPointF &first = stroke.points.first();
                strokePath.moveTo(first.x() * m_scale, first.y() * m_scale);
                for (int i = 1; i < stroke.points.size(); ++i) {
                    const QPointF &point = stroke.points.at(i);
                    strokePath.lineTo(point.x() * m_scale, point.y() * m_scale);
                }

                const qreal width = qMax(0.1, stroke.style.widthPoints * m_scale);
                if (stroke.points.size() == 1) {
                    const QPointF point = strokePath.currentPosition();
                    erasedInk.addEllipse(point, width / 2.0, width / 2.0);
                } else {
                    QPainterPathStroker stroker;
                    stroker.setWidth(width);
                    stroker.setCapStyle(Qt::RoundCap);
                    stroker.setJoinStyle(Qt::RoundJoin);
                    erasedInk.addPath(stroker.createStroke(strokePath));
                }
            }
        };
        if (exactPageImage && m_erasing && m_erasePage == page) {
            QVector<Stroke> erasedStrokes;
            for (const ErasePreviewStroke &preview : std::as_const(m_erasePreviewStrokes)) {
                if (preview.erased)
                    erasedStrokes.append(preview.stroke);
            }
            addErasedStrokes(erasedStrokes);
        }
        if (exactPageImage && m_pendingErasedPage == page) {
            addErasedStrokes(m_pendingErasedStrokes);
        }
        if (!renderedPage.isNull())
            painter.drawPixmap(target, renderedPage, renderedPage.rect());
        if (((m_erasing && m_erasePage == page)
             || m_pendingErasedPage == page)
            && exactPageImage && !erasedInk.isEmpty()) {
            const QPixmap basePage = pageImage(page, target.size(), dpr, false);
            painter.save();
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            painter.setClipPath(erasedInk.translated(target.topLeft()),
                                Qt::IntersectClip);
            painter.drawPixmap(target, basePage, basePage.rect());
            painter.restore();
        }
        drawSearchResults(painter, page, target, current);
        drawFormFieldOverlays(painter, page, target);

        painter.save();
        painter.setClipRect(target, Qt::IntersectClip);

        for (const SelectionPart &part : std::as_const(m_selection)) {
            if (part.page != page)
                continue;
            painter.save();
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 120, 215, 90));
            for (const QPolygonF &polygon : part.bounds) {
                QPolygonF mapped;
                for (const QPointF &p : polygon)
                    mapped << QPointF(target.x() + p.x() * m_scale,
                                      target.y() + p.y() * m_scale);
                painter.drawPolygon(mapped);
            }
            painter.restore();
        }

        const auto drawTextPreview = [this, &painter, page, target, dpr](
                                         const QRectF &oldBounds,
                                         const QRectF &newBounds,
                                         const QString &text,
                                         const TextStyle &style) {
            if (!oldBounds.isEmpty()) {
                const QPixmap basePage = pageImage(page, target.size(), dpr, false);
                if (!basePage.isNull()) {
                    const QRectF oldRect(
                        target.x() + oldBounds.x() * m_scale,
                        target.y() + oldBounds.y() * m_scale,
                        oldBounds.width() * m_scale, oldBounds.height() * m_scale);
                    painter.save();
                    painter.setCompositionMode(QPainter::CompositionMode_Source);
                    painter.setClipRect(oldRect, Qt::IntersectClip);
                    painter.drawPixmap(target, basePage, basePage.rect());
                    painter.restore();
                }
            }

            const QRectF textBounds(
                target.x() + newBounds.x() * m_scale,
                target.y() + newBounds.y() * m_scale,
                newBounds.width() * m_scale, newBounds.height() * m_scale);
            QFont font(style.family);
            font.setPixelSize(qMax(1, qRound(style.sizePoints * m_scale)));
            painter.save();
            painter.setFont(font);
            painter.setPen(style.color);
            painter.drawText(textBounds, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                             text);
            painter.restore();
        };
        if ((m_movingTextAnnotation || m_resizingTextAnnotation)
            && m_selectedTextAnnotationPage == page) {
            drawTextPreview(m_textAnnotationResizeOriginalBounds,
                            m_selectedTextAnnotationBounds,
                            m_selectedTextAnnotationContents,
                            m_selectedTextAnnotationStyle);
        }
        if (m_pendingTextAnnotationPreview.page == page) {
            drawTextPreview(m_pendingTextAnnotationPreview.oldBounds,
                            m_pendingTextAnnotationPreview.newBounds,
                            m_pendingTextAnnotationPreview.text,
                            m_pendingTextAnnotationPreview.style);
        }

        const auto drawImageStampPreview = [this, &painter, page, target, dpr](
                                               const QRectF &oldPageBounds,
                                               const QRectF &newPageBounds,
                                               const QImage &image) {
            const QRectF oldViewportBounds(
                target.x() + oldPageBounds.x() * m_scale,
                target.y() + oldPageBounds.y() * m_scale,
                oldPageBounds.width() * m_scale,
                oldPageBounds.height() * m_scale);
            const QPixmap basePage = pageImage(page, target.size(), dpr, false);
            if (!basePage.isNull()) {
                painter.save();
                painter.setCompositionMode(QPainter::CompositionMode_Source);
                painter.setClipRect(oldViewportBounds, Qt::IntersectClip);
                painter.drawPixmap(target, basePage, basePage.rect());
                painter.restore();
            }
            if (!newPageBounds.isEmpty() && !image.isNull()) {
                const QRectF imageBounds(
                    target.x() + newPageBounds.x() * m_scale,
                    target.y() + newPageBounds.y() * m_scale,
                    newPageBounds.width() * m_scale,
                    newPageBounds.height() * m_scale);
                painter.save();
                painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
                painter.drawImage(imageBounds, image);
                painter.restore();
            }
        };
        if (m_selectedImagePage == page) {
            if ((m_movingImageStamp || m_resizingImageStamp)
                && !m_selectedImagePixmap.isNull()) {
                drawImageStampPreview(m_imageStampOriginalBounds,
                                      m_selectedImageBounds,
                                      m_selectedImagePixmap);
            }
            const QRectF imageBounds(
                target.x() + m_selectedImageBounds.x() * m_scale,
                target.y() + m_selectedImageBounds.y() * m_scale,
                m_selectedImageBounds.width() * m_scale,
                m_selectedImageBounds.height() * m_scale);
            painter.save();
            painter.setPen(QPen(QColor(0, 102, 204), 2, Qt::DashLine));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(imageBounds);
            painter.setPen(QPen(QColor(0, 102, 204), 1));
            painter.setBrush(Qt::white);
            const QPointF corners[] = {
                imageBounds.topLeft(), imageBounds.topRight(),
                imageBounds.bottomLeft(), imageBounds.bottomRight()
            };
            for (const QPointF &corner : corners)
                painter.drawRect(QRectF(corner.x() - 4, corner.y() - 4, 8, 8));
            painter.restore();
        }
        if (m_pendingImageStampPreview.page == page) {
            drawImageStampPreview(m_pendingImageStampPreview.oldBounds,
                                  m_pendingImageStampPreview.newBounds,
                                  m_pendingImageStampPreview.image);
        }

        if (m_selectedTextAnnotationPage == page) {
            const QRectF bounds(
                target.x() + m_selectedTextAnnotationBounds.x() * m_scale,
                target.y() + m_selectedTextAnnotationBounds.y() * m_scale,
                m_selectedTextAnnotationBounds.width() * m_scale,
                m_selectedTextAnnotationBounds.height() * m_scale);
            painter.save();
            painter.setPen(QPen(QColor(0, 102, 204), 2, Qt::DashLine));
            painter.setBrush(QColor(0, 102, 204, 35));
            painter.drawRect(bounds);
            painter.setPen(QPen(QColor(0, 102, 204), 1));
            painter.setBrush(Qt::white);
            const QPointF corners[] = {
                bounds.topLeft(), bounds.topRight(),
                bounds.bottomLeft(), bounds.bottomRight()
            };
            for (const QPointF &corner : corners)
                painter.drawRect(QRectF(corner.x() - 4, corner.y() - 4, 8, 8));
            painter.restore();
        }

        for (const Stroke &stroke : std::as_const(m_strokes)) {
            if (stroke.page == page)
                drawStroke(painter, stroke, target);
        }
        for (const Stroke &stroke : std::as_const(m_pendingCommittedStrokes)) {
            if (stroke.page == page)
                drawStroke(painter, stroke, target);
        }
        if (m_drawing && m_activeStroke.page == page)
            drawStroke(painter, m_activeStroke, target);

        if (m_imageActive && m_imagePage == page)
            drawImagePlacement(painter, target);

        if (m_drawTool && m_eraserActive && m_erasePage == page) {
            const QPointF center(m_eraserCursorPos);
            const qreal radius = qMax(4.0, m_drawStyle.widthPoints * m_scale / 2.0);
            painter.save();
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(QPen(QColor(40, 40, 40, 190), 1));
            painter.setBrush(QColor(255, 255, 255, 100));
            painter.drawEllipse(center, radius, radius);
            painter.restore();
        }

        const auto drawRedactionPreview = [&painter, &target, this](const QRectF &bounds) {
            const QRectF pixelBounds(
                target.x() + bounds.x() * m_scale,
                target.y() + bounds.y() * m_scale,
                bounds.width() * m_scale,
                bounds.height() * m_scale);
            painter.save();
            painter.setPen(QPen(QColor(120, 0, 0), 2, Qt::DashLine));
            painter.setBrush(QColor(180, 0, 0, 150));
            painter.drawRect(pixelBounds);
            painter.restore();
        };
        for (const PdfEditor::RedactionArea &area : std::as_const(m_redactionAreas)) {
            if (area.page == page)
                drawRedactionPreview(area.bounds);
        }
        if (m_redactionDrawing && m_redactionPage == page
            && !m_redactionPreview.isEmpty()) {
            drawRedactionPreview(m_redactionPreview);
        }

        painter.restore();
    }

    if (m_dragging && !m_dragRect.isNull()) {
        painter.setPen(QPen(QColor(0, 102, 204), 2, Qt::DashLine));
        painter.setBrush(QColor(0, 102, 204, 40));
        painter.drawRect(m_dragRect);
    }

    drawLoadingIndicator(painter);
}

void PageView::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    recalculateLayout(true);
}

void PageView::scrollContentsBy(int dx, int dy)
{
    QAbstractScrollArea::scrollContentsBy(dx, dy);
    positionEditor();
    positionFormEditor();
    if (!m_jumping)
        updateCurrentPage();
}

void PageView::wheelEvent(QWheelEvent *event)
{
    if (m_loading || m_interactionBlocked) {
        event->accept();
        return;
    }

    if (m_pressedLinkPage >= 0 && event->button() == Qt::LeftButton) {
        const int pressedPage = m_pressedLinkPage;
        const QUrl pressedUrl = m_pressedLinkUrl;
        const int destinationPage = m_pressedLinkDestinationPage;
        const QPointF destinationLocation = m_pressedLinkLocation;
        m_pressedLinkPage = -1;
        m_pressedLinkUrl = QUrl();
        m_pressedLinkDestinationPage = -1;
        m_pressedLinkLocation = QPointF();

        const int releasePage = pageAtViewportPoint(event->position().toPoint());
        if (releasePage == pressedPage) {
            if (!pressedUrl.isEmpty())
                emit externalLinkRequested(pressedUrl);
            else if (destinationPage >= 0)
                jumpToPage(destinationPage, destinationLocation);
        }
        viewport()->setCursor(Qt::ArrowCursor);
        event->accept();
        return;
    }

    if (event->modifiers() & Qt::ControlModifier) {
        const double currentZoom = m_zoomTimer->isActive()
                                       ? m_pendingZoomFactor
                                       : zoomFactor();
        m_pendingZoomFactor = currentZoom
                              * (event->angleDelta().y() > 0 ? 1.1 : 1.0 / 1.1);
        m_zoomTimer->start();
        event->accept();
        return;
    }
    QAbstractScrollArea::wheelEvent(event);
}

void PageView::keyPressEvent(QKeyEvent *event)
{
    if (m_loading || m_interactionBlocked) {
        event->accept();
        return;
    }

    if (m_imageAwaiting || m_imageActive) {
        if (event->key() == Qt::Key_Escape) {
            finishImage(false);
            return;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            finishImage(true);
            return;
        }
    }

    if (event->matches(QKeySequence::Copy) && !m_selection.isEmpty()) {
        copySelection();
        return;
    }
    if (event->matches(QKeySequence::SelectAll)) {
        selectAllText();
        return;
    }
    if (event->key() == Qt::Key_Escape && !m_selection.isEmpty()) {
        clearSelection();
        return;
    }
    if (event->key() == Qt::Key_Escape && m_selectedTextAnnotationPage >= 0) {
        clearSelectedTextAnnotation();
        return;
    }
    if (event->key() == Qt::Key_Escape && m_selectedImagePage >= 0) {
        clearSelectedImageStamp();
        return;
    }
    if (event->key() == Qt::Key_Escape && m_redactionTool) {
        setRedactionToolActive(false);
        return;
    }
    if (event->key() == Qt::Key_Delete && m_selectedTextAnnotationPage >= 0) {
        deleteSelectedTextAnnotation();
        return;
    }
    if (event->key() == Qt::Key_Delete && m_selectedImagePage >= 0) {
        deleteSelectedImageStamp();
        return;
    }
    QAbstractScrollArea::keyPressEvent(event);
}

void PageView::commitPendingEdits()
{
    finishEditor(true);
    finishFormEditor(true);
    commitStrokes();
    finishImage(true);
}

// text selection

void PageView::updateSelection()
{
    m_selection.clear();
    if (!m_document || m_selStartPage < 0 || m_selEndPage < 0)
        return;

    int firstPage = m_selStartPage;
    int lastPage = m_selEndPage;
    QPointF a = m_selStartPoint;
    QPointF b = m_selEndPoint;
    const bool sameLine = qAbs(b.y() - a.y()) < 3.0;
    if (lastPage < firstPage
        || (lastPage == firstPage && (b.y() < a.y() || (sameLine && b.x() < a.x())))) {
        std::swap(firstPage, lastPage);
        std::swap(a, b);
    }

    for (int page = firstPage; page <= lastPage; ++page) {
        const QSizeF size = m_pageSizes.at(page);
        const QPointF start = (page == firstPage) ? a : QPointF(0, 0);
        const QPointF end = (page == lastPage) ? b : QPointF(size.width(), size.height());

        const PdfEditor::TextSelection selection =
            m_document->selectText(page, start, end);
        if (!selection.text.isEmpty()) {
            SelectionPart part;
            part.page = page;
            part.bounds = selection.bounds;
            part.text = selection.text;
            m_selection.append(part);
        }
    }
}

void PageView::extendSelection(const QPoint &viewportPos)
{
    if (!m_selMoved) {
        if ((viewportPos - m_selPressPos).manhattanLength() < QApplication::startDragDistance())
            return;
        m_selMoved = true;
    }

    int page = pageAtViewportPoint(viewportPos);
    if (page < 0)
        page = nearestPage(viewportPos);
    m_selEndPage = page;
    m_selEndPoint = pagePointFor(page, viewportPos);

    updateSelection();
    viewport()->update();
}

void PageView::selectWordAt(int page, const QPointF &point)
{
    if (!m_document || page < 0 || page >= m_pageSizes.size())
        return;

    const PdfEditor::TextSelection word = m_document->selectWord(page, point);
    if (word.text.isEmpty())
        return;

    m_selection.clear();
    SelectionPart part;
    part.page = page;
    part.bounds = word.bounds;
    part.text = word.text;
    m_selection.append(part);
    m_selStartPage = page;
    m_selEndPage = page;
    m_selStartPoint = point;
    m_selEndPoint = point;
    m_selMoved = true;
    m_selecting = false;
    m_autoScroll->stop();
    viewport()->update();
    emit textSelected();
}

void PageView::clearSelection()
{
    m_selStartPage = -1;
    m_selEndPage = -1;
    if (m_selection.isEmpty())
        return;
    m_selection.clear();
    viewport()->update();
}

void PageView::selectAllText()
{
    if (m_pageSizes.isEmpty())
        return;
    const int last = int(m_pageSizes.size()) - 1;
    m_selStartPage = 0;
    m_selStartPoint = QPointF(0, 0);
    m_selEndPage = last;
    m_selEndPoint = QPointF(m_pageSizes.at(last).width(), m_pageSizes.at(last).height());

    updateSelection();
    viewport()->update();
    if (!m_selection.isEmpty())
        emit textSelected();
}

void PageView::copySelection()
{
    if (m_selection.isEmpty())
        return;

    QStringList parts;
    for (const SelectionPart &part : std::as_const(m_selection))
        parts << part.text;

    QString text = parts.join(QLatin1Char('\n'));
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    QGuiApplication::clipboard()->setText(text);
}

void PageView::contextMenuEvent(QContextMenuEvent *event)
{
    if (m_loading)
        return;

    if (m_textTool || m_drawTool || m_redactionTool || m_editor
        || m_imageAwaiting || m_imageActive)
        return;

    const int page = pageAtViewportPoint(event->pos());
    int annotationId = 0;
    QRectF annotationBounds;
    QString annotationText;
    TextStyle annotationStyle;
    const bool onAnnotation = page >= 0 && m_textAnnotationHandler
        && m_textAnnotationHandler(page, pagePointFor(page, event->pos()),
                                   &annotationId, &annotationBounds, &annotationText,
                                   &annotationStyle);
    if (onAnnotation) {
        clearSelectedImageStamp();
        selectTextAnnotation(page, annotationId, annotationBounds,
                             annotationText, annotationStyle);
    } else {
        clearSelectedTextAnnotation();
    }

    QMenu menu(this);
    QAction *editAnnotation = nullptr;
    QAction *deleteAnnotation = nullptr;
    QAction *deleteImage = nullptr;
    if (m_selectedTextAnnotationPage >= 0) {
        editAnnotation = menu.addAction("Edit added text...");
        deleteAnnotation = menu.addAction("Delete added text");
        menu.addSeparator();
    } else if (page >= 0 && m_imageStampHandler) {
        PdfEditor::ImageStamp stamp;
        if (m_imageStampHandler(page, pagePointFor(page, event->pos()), &stamp)) {
            selectImageStamp(page, stamp);
            deleteImage = menu.addAction("Delete added image");
            menu.addSeparator();
        } else {
            clearSelectedImageStamp();
        }
    }
    QAction *copy = menu.addAction("Copy");
    copy->setShortcut(QKeySequence::Copy);
    copy->setEnabled(!m_selection.isEmpty());
    QAction *all = menu.addAction("Select all");
    all->setShortcut(QKeySequence::SelectAll);

    const QAction *chosen = menu.exec(event->globalPos());
    if (chosen && chosen == editAnnotation) {
        emit textAnnotationEditRequested(m_selectedTextAnnotationPage,
                                         m_selectedTextAnnotationId,
                                         m_selectedTextAnnotationContents);
    } else if (chosen && chosen == deleteAnnotation) {
        deleteSelectedTextAnnotation();
    } else if (chosen && chosen == deleteImage) {
        deleteSelectedImageStamp();
    } else if (chosen && chosen == copy) {
        copySelection();
    } else if (chosen && chosen == all) {
        selectAllText();
    }
}

// draw tool

void PageView::setDrawToolActive(bool active)
{
    if (!active)
        commitStrokes();
    else
        clearSelection();
    m_drawTool = active;
    m_drawing = false;
    if (active)
        viewport()->setCursor(Qt::CrossCursor);
    else if (!m_textTool)
        viewport()->setCursor(Qt::ArrowCursor);
    viewport()->update();
}

void PageView::setRedactionToolActive(bool active)
{
    if (m_redactionTool == active)
        return;

    m_redactionTool = active;
    m_redactionDrawing = false;
    m_redactionPage = -1;
    m_redactionPreview = QRectF();
    m_pressedLinkPage = -1;
    m_pressedLinkUrl = QUrl();
    m_pressedLinkDestinationPage = -1;
    m_pressedLinkLocation = QPointF();
    if (active)
        clearSelection();
    viewport()->setCursor(active ? Qt::CrossCursor
                                 : (m_textTool ? Qt::IBeamCursor : Qt::ArrowCursor));
    viewport()->update();
    emit redactionToolChanged(active);
}

void PageView::clearRedactionAreas()
{
    if (m_redactionAreas.isEmpty())
        return;
    m_redactionAreas.clear();
    viewport()->update();
    emit redactionAreasChanged(0);
}

void PageView::setEraserActive(bool active)
{
    m_eraserActive = active;
    m_erasing = false;
    m_erasePoints.clear();
    m_erasePreviewStrokes.clear();
    m_erasePage = -1;
    viewport()->setCursor(m_drawTool ? Qt::CrossCursor : Qt::ArrowCursor);
    viewport()->update();
}

void PageView::setInkStrokeHandler(
    const std::function<bool(int, QVector<Stroke> *)> &handler)
{
    m_inkStrokeHandler = handler;
}

void PageView::updateErasePreview(const QPointF &start, const QPointF &end,
                                  QRectF &changedBounds)
{
    const QRectF eraserBounds = QRectF(start, end).normalized();
    const double eraserRadius = m_drawStyle.widthPoints / 2.0;
    for (ErasePreviewStroke &preview : m_erasePreviewStrokes) {
        if (preview.erased)
            continue;
        const double hitRadius = eraserRadius + preview.stroke.style.widthPoints / 2.0;
        if (!preview.bounds.intersects(
                eraserBounds.adjusted(-hitRadius, -hitRadius, hitRadius, hitRadius)))
            continue;

        const double hitRadiusSquared = hitRadius * hitRadius;
        const QVector<QPointF> &points = preview.stroke.points;
        if (points.size() == 1) {
            preview.erased = pointSegmentDistanceSquared(
                points.first(), start, end) <= hitRadiusSquared;
        } else {
            for (int i = 1; i < points.size(); ++i) {
                if (segmentDistanceSquared(start, end, points.at(i - 1), points.at(i))
                    <= hitRadiusSquared) {
                    preview.erased = true;
                    break;
                }
            }
        }
        if (preview.erased) {
            changedBounds = changedBounds.isNull()
                                ? preview.bounds
                                : changedBounds.united(preview.bounds);
        }
    }
}

QRect PageView::erasePreviewViewportRect(const QRectF &bounds) const
{
    if (m_erasePage < 0 || m_erasePage >= m_pageRects.size() || bounds.isNull())
        return {};

    const QRect pageRect = pageRectInViewport(m_erasePage);
    return QRectF(pageRect.topLeft()
                      + QPointF(bounds.x() * m_scale, bounds.y() * m_scale),
                  QSizeF(bounds.width() * m_scale, bounds.height() * m_scale))
        .adjusted(-2, -2, 2, 2).toAlignedRect().intersected(pageRect);
}

void PageView::setDrawStyle(const DrawStyle &style)
{
    m_drawStyle = style;
}

bool PageView::undoLastStroke()
{
    if (m_strokes.isEmpty())
        return false;
    m_strokes.removeLast();
    viewport()->update();
    emit pendingStrokesChanged();
    emit pendingChangesChanged();
    return true;
}

void PageView::commitStrokes()
{
    if (m_strokes.isEmpty())
        return;
    const QVector<Stroke> strokes = m_strokes;
    preserveCommittedStrokes(strokes);
    m_strokes.clear();
    viewport()->update();
    emit pendingStrokesChanged();
    emit pendingChangesChanged();
    emit strokesPlaced(strokes);
}

void PageView::preserveCommittedStrokes(const QVector<Stroke> &strokes)
{
    m_pendingCommittedStrokes += strokes;
    viewport()->update();
}

void PageView::preserveErasedStrokes(int page, const QVector<Stroke> &strokes)
{
    if (strokes.isEmpty())
        return;
    m_pendingErasedPage = page;
    m_pendingErasedStrokes = strokes;
    viewport()->update();
}

void PageView::preserveTextAnnotationChange(int page, const QRectF &oldBounds,
                                            const QRectF &newBounds, const QString &text,
                                            const TextStyle &style)
{
    m_pendingTextAnnotationPreview = { page, oldBounds, newBounds, text, style };
    viewport()->update();
}

// image placement

void PageView::beginImagePlacement(const QImage &image)
{
    finishImage(false);
    if (image.isNull())
        return;
    clearSelectedImageStamp();
    clearSelectedTextAnnotation();
    clearSelection();
    m_pendingImage = image;
    m_imageAwaiting = true;
    viewport()->setCursor(Qt::CrossCursor);
    setFocus();
    emit imagePlacementStateChanged(true, false);
}

void PageView::setImageCropMode(bool active)
{
    if (!m_imageActive || m_imageCropping == active)
        return;

    m_imageCropping = active;
    m_imageCropDragging = false;
    m_imageCropRect = active ? m_imageRect : QRectF();
    viewport()->setCursor(active ? Qt::CrossCursor : Qt::SizeAllCursor);
    emit imageCropModeChanged(active);
    viewport()->update();
}

void PageView::cancelImagePlacement()
{
    finishImage(false);
}

void PageView::finishImage(bool commit)
{
    if (!m_imageAwaiting && !m_imageActive)
        return;

    const bool wasActive = m_imageActive;
    const bool wasCropping = m_imageCropping;
    const QImage image = m_pendingImage;
    const int page = m_imagePage;
    const QRectF box = m_imageRect;

    m_imageAwaiting = false;
    m_imageActive = false;
    m_imageCropping = false;
    m_imageCropDragging = false;
    m_imageCropRect = QRectF();
    m_imageDrag = ImageDrag::None;
    m_pendingImage = QImage();
    viewport()->setCursor(Qt::ArrowCursor);
    viewport()->update();

    if (commit && wasActive)
        emit imagePlaced(page, box, image);
    emit imagePlacementStateChanged(false, false);
    if (wasCropping)
        emit imageCropModeChanged(false);
    emit pendingChangesChanged();
}

QRectF PageView::imagePixelRect() const
{
    const QRect page = pageRectInViewport(m_imagePage);
    return QRectF(page.x() + m_imageRect.x() * m_scale, page.y() + m_imageRect.y() * m_scale,
                  m_imageRect.width() * m_scale, m_imageRect.height() * m_scale);
}

void PageView::updateImageCropRect(const QPoint &viewportPos)
{
    const QRect page = pageRectInViewport(m_imagePage);
    const QPointF imagePoint((viewportPos.x() - page.x()) / m_scale,
                             (viewportPos.y() - page.y()) / m_scale);
    const QPointF end(qBound(m_imageRect.left(), imagePoint.x(), m_imageRect.right()),
                      qBound(m_imageRect.top(), imagePoint.y(), m_imageRect.bottom()));
    m_imageCropRect = QRectF(m_imageCropAnchor, end).normalized().intersected(m_imageRect);
    viewport()->update();
}

QRectF PageView::imageCropPixelRect() const
{
    const QRect page = pageRectInViewport(m_imagePage);
    const QRectF crop = m_imageCropRect.isEmpty() ? m_imageRect : m_imageCropRect;
    return QRectF(page.x() + crop.x() * m_scale, page.y() + crop.y() * m_scale,
                  crop.width() * m_scale, crop.height() * m_scale);
}

int PageView::imageCropHandleAt(const QPoint &viewportPos) const
{
    const QRectF rect = imageCropPixelRect();
    const QPointF corners[] = {
        rect.topLeft(), rect.topRight(), rect.bottomLeft(), rect.bottomRight()
    };
    for (int i = 0; i < 4; ++i) {
        if (QLineF(corners[i], QPointF(viewportPos)).length() <= 12.0)
            return i;
    }
    return -1;
}

void PageView::applyImageCrop()
{
    if (m_imageCropRect.width() < 3.0 || m_imageCropRect.height() < 3.0) {
        m_imageCropRect = QRectF();
        viewport()->update();
        return;
    }

    const QRectF selected = m_imageCropRect.intersected(m_imageRect);
    if (selected.width() < 3.0 || selected.height() < 3.0) {
        m_imageCropRect = m_imageRect;
        viewport()->update();
        return;
    }
    const double left = (selected.left() - m_imageRect.left()) / m_imageRect.width();
    const double top = (selected.top() - m_imageRect.top()) / m_imageRect.height();
    const double right = (selected.right() - m_imageRect.left()) / m_imageRect.width();
    const double bottom = (selected.bottom() - m_imageRect.top()) / m_imageRect.height();

    const int sourceWidth = m_pendingImage.width();
    const int sourceHeight = m_pendingImage.height();
    const int x = qBound(0, qFloor(left * sourceWidth), sourceWidth - 1);
    const int y = qBound(0, qFloor(top * sourceHeight), sourceHeight - 1);
    const int cropRight = qBound(x + 1, qCeil(right * sourceWidth), sourceWidth);
    const int cropBottom = qBound(y + 1, qCeil(bottom * sourceHeight), sourceHeight);
    const QRect sourceCrop(x, y, cropRight - x, cropBottom - y);
    const QRectF oldPageRect = m_imageRect;
    const QImage cropped = m_pendingImage.copy(sourceCrop);
    if (cropped.isNull())
        return;

    m_imageRect = QRectF(oldPageRect.left() + left * oldPageRect.width(),
                         oldPageRect.top() + top * oldPageRect.height(),
                         (right - left) * oldPageRect.width(),
                         (bottom - top) * oldPageRect.height());
    m_pendingImage = cropped;
    m_imageCropRect = m_imageRect;
    viewport()->setCursor(Qt::CrossCursor);
    emit pendingChangesChanged();
    viewport()->update();
}

int PageView::imageHandleAt(const QPoint &viewportPos) const
{
    if (m_imagePage < 0 || m_imagePage >= m_pageRects.size())
        return -1;
    const QRectF r = imagePixelRect();
    const QPointF corners[4] = { r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight() };
    for (int i = 0; i < 4; ++i) {
        if (QLineF(corners[i], QPointF(viewportPos)).length() <= 12.0)
            return i;
    }
    return -1;
}

void PageView::updateImageCursor(const QPoint &viewportPos)
{
    const int handle = m_imageCropping
                           ? imageCropHandleAt(viewportPos) : imageHandleAt(viewportPos);
    if (handle == 0 || handle == 3)
        viewport()->setCursor(Qt::SizeFDiagCursor);
    else if (handle == 1 || handle == 2)
        viewport()->setCursor(Qt::SizeBDiagCursor);
    else if (imagePixelRect().contains(QPointF(viewportPos)))
        viewport()->setCursor(Qt::SizeAllCursor);
    else
        viewport()->setCursor(Qt::ArrowCursor);
}

void PageView::drawImagePlacement(QPainter &painter, const QRect &pageRect)
{
    const QRectF px(pageRect.x() + m_imageRect.x() * m_scale,
                    pageRect.y() + m_imageRect.y() * m_scale,
                    m_imageRect.width() * m_scale, m_imageRect.height() * m_scale);

    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(px, m_pendingImage);

    if (m_imageCropping) {
        const QRectF cropRect = imageCropPixelRect();
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 125));
        painter.drawRect(QRectF(px.left(), px.top(), px.width(), cropRect.top() - px.top()));
        painter.drawRect(QRectF(px.left(), cropRect.bottom(),
                                px.width(), px.bottom() - cropRect.bottom()));
        painter.drawRect(QRectF(px.left(), cropRect.top(),
                                cropRect.left() - px.left(), cropRect.height()));
        painter.drawRect(QRectF(cropRect.right(), cropRect.top(),
                                px.right() - cropRect.right(), cropRect.height()));
        painter.setPen(QPen(Qt::white, 2, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(cropRect);
        painter.setPen(QPen(QColor(0, 102, 204), 1));
        painter.setBrush(Qt::white);
        const QPointF cropCorners[] = {
            cropRect.topLeft(), cropRect.topRight(),
            cropRect.bottomLeft(), cropRect.bottomRight()
        };
        for (const QPointF &corner : cropCorners)
            painter.drawRect(QRectF(corner.x() - 5, corner.y() - 5, 10, 10));
    }

    if (m_imageCropping) {
        painter.restore();
        return;
    }

    painter.setPen(QPen(QColor(0, 102, 204), 2, Qt::DashLine));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(px);
    painter.setPen(QPen(QColor(0, 102, 204), 1));
    painter.setBrush(Qt::white);
    const QPointF corners[4] = { px.topLeft(), px.topRight(), px.bottomLeft(), px.bottomRight() };
    for (const QPointF &c : corners)
        painter.drawRect(QRectF(c.x() - 5, c.y() - 5, 10, 10));
    painter.restore();
}

// text tool

void PageView::setTextToolActive(bool active)
{
    if (!active)
        finishEditor(true);
    else
        clearSelection();
    m_textTool = active;
    m_dragging = false;
    m_dragRect = QRect();
    if (active)
        viewport()->setCursor(Qt::IBeamCursor);
    else if (!m_drawTool)
        viewport()->setCursor(Qt::ArrowCursor);
    viewport()->update();
}

void PageView::setTextStyle(const TextStyle &style)
{
    m_style = style;
    if (m_editor) {
        if (m_editingTextAnnotation
            && (m_editorStyle.family != style.family
                || !qFuzzyCompare(m_editorStyle.sizePoints, style.sizePoints)
                || m_editorStyle.color != style.color))
            m_editingTextAnnotationStyleChanged = true;
        m_editorStyle = style;
        applyEditorStyle();
    }
}

void PageView::setFormFieldHandler(
    const std::function<bool(int, const QPointF &, FormField *)> &handler)
{
    m_formFieldHandler = handler;
}

void PageView::setFormFieldValueHandler(
    const std::function<bool(int, int, const QString &)> &handler)
{
    m_formFieldValueHandler = handler;
}

void PageView::setTextAnnotationHandler(
    const std::function<bool(int, const QPointF &, int *, QRectF *, QString *,
                             TextStyle *)> &handler)
{
    m_textAnnotationHandler = handler;
}

void PageView::setImageStampHandler(
    const std::function<bool(int, const QPointF &, PdfEditor::ImageStamp *)> &handler)
{
    m_imageStampHandler = handler;
}

void PageView::editTextAnnotationInline(int page, int annotationId, const QString &text)
{
    if (m_selectedTextAnnotationPage != page
        || m_selectedTextAnnotationId != annotationId)
        return;

    startAnnotationEditor(page, annotationId, m_selectedTextAnnotationBounds,
                          text, m_selectedTextAnnotationStyle);
}

void PageView::refreshSelectedTextAnnotation()
{
    if (m_selectedTextAnnotationPage < 0)
        return;

    int annotationId = 0;
    QRectF bounds;
    QString text;
    TextStyle style;
    const QPointF center = m_selectedTextAnnotationBounds.center();
    if (m_textAnnotationHandler
        && m_textAnnotationHandler(m_selectedTextAnnotationPage, center,
                                   &annotationId, &bounds, &text, &style)) {
        selectTextAnnotation(m_selectedTextAnnotationPage, annotationId,
                             bounds, text, style);
    } else {
        clearSelectedTextAnnotation();
    }
}

void PageView::deleteSelectedTextAnnotation()
{
    if (m_selectedTextAnnotationPage < 0)
        return;

    if (m_editor && m_editingTextAnnotation)
        finishEditor(false);

    const int page = m_selectedTextAnnotationPage;
    const int annotationId = m_selectedTextAnnotationId;
    const QString text = m_selectedTextAnnotationContents;
    clearSelectedTextAnnotation();
    emit textAnnotationDeleteRequested(page, annotationId, text);
}

void PageView::deleteSelectedImageStamp()
{
    if (m_selectedImagePage < 0)
        return;

    const int page = m_selectedImagePage;
    const QString id = m_selectedImageId;
    m_pendingImageStampPreview = {
        page, m_selectedImageBounds, QRectF(), m_selectedImagePixmap
    };
    clearSelectedImageStamp();
    emit imageStampDeleteRequested(page, id);
}

void PageView::selectImageStamp(int page, const PdfEditor::ImageStamp &stamp)
{
    const bool wasSelected = m_selectedImagePage >= 0;
    m_selectedImagePage = page;
    m_selectedImageId = stamp.id;
    m_selectedImageBounds = stamp.bounds;
    m_imageStampOriginalBounds = stamp.bounds;
    m_selectedImagePixmap = stamp.image;
    if (!wasSelected)
        emit imageStampSelectionChanged(true);
    viewport()->update();
}

void PageView::clearSelectedImageStamp()
{
    if (m_selectedImagePage < 0)
        return;
    m_selectedImagePage = -1;
    m_selectedImageId.clear();
    m_selectedImageBounds = QRectF();
    m_imageStampOriginalBounds = QRectF();
    m_selectedImagePixmap = QImage();
    m_movingImageStamp = false;
    m_resizingImageStamp = false;
    m_imageStampResizeCorner = -1;
    emit imageStampSelectionChanged(false);
    viewport()->update();
}

void PageView::selectTextAnnotation(int page, int annotationId, const QRectF &bounds,
                                    const QString &text, const TextStyle &style)
{
    const bool wasSelected = m_selectedTextAnnotationPage >= 0;
    m_selectedTextAnnotationPage = page;
    m_selectedTextAnnotationId = annotationId;
    m_selectedTextAnnotationBounds = bounds;
    m_selectedTextAnnotationContents = text;
    m_selectedTextAnnotationStyle = style;
    if (!wasSelected)
        emit textAnnotationSelectionChanged(true);
    viewport()->update();
}

void PageView::clearSelectedTextAnnotation()
{
    if (m_selectedTextAnnotationPage < 0)
        return;
    m_selectedTextAnnotationPage = -1;
    m_selectedTextAnnotationId = 0;
    m_selectedTextAnnotationBounds = QRectF();
    m_selectedTextAnnotationContents.clear();
    m_selectedTextAnnotationStyle = TextStyle();
    emit textAnnotationSelectionChanged(false);
    viewport()->update();
}

QRectF PageView::selectedTextAnnotationViewportBounds() const
{
    if (m_selectedTextAnnotationPage < 0
        || m_selectedTextAnnotationPage >= m_pageRects.size())
        return {};
    const QRect page = pageRectInViewport(m_selectedTextAnnotationPage);
    return QRectF(page.x() + m_selectedTextAnnotationBounds.x() * m_scale,
                  page.y() + m_selectedTextAnnotationBounds.y() * m_scale,
                  m_selectedTextAnnotationBounds.width() * m_scale,
                  m_selectedTextAnnotationBounds.height() * m_scale);
}

QRectF PageView::selectedImageViewportBounds() const
{
    if (m_selectedImagePage < 0 || m_selectedImagePage >= m_pageRects.size())
        return {};
    const QRect page = pageRectInViewport(m_selectedImagePage);
    return QRectF(page.x() + m_selectedImageBounds.x() * m_scale,
                  page.y() + m_selectedImageBounds.y() * m_scale,
                  m_selectedImageBounds.width() * m_scale,
                  m_selectedImageBounds.height() * m_scale);
}

int PageView::imageStampResizeHandleAt(const QPoint &pos) const
{
    const QRectF bounds = selectedImageViewportBounds();
    if (bounds.isEmpty())
        return -1;
    const QPointF corners[] = {
        bounds.topLeft(), bounds.topRight(), bounds.bottomLeft(), bounds.bottomRight()
    };
    for (int i = 0; i < 4; ++i) {
        if (QRectF(corners[i] - QPointF(7, 7), QSizeF(14, 14)).contains(pos))
            return i;
    }
    return -1;
}

int PageView::textAnnotationResizeHandleAt(const QPoint &pos) const
{
    const QRectF bounds = selectedTextAnnotationViewportBounds();
    if (bounds.isEmpty())
        return -1;
    const QPointF corners[] = {
        bounds.topLeft(), bounds.topRight(), bounds.bottomLeft(), bounds.bottomRight()
    };
    for (int i = 0; i < 4; ++i) {
        if (QRectF(corners[i] - QPointF(7, 7), QSizeF(14, 14)).contains(pos))
            return i;
    }
    return -1;
}

void PageView::setFormFields(int page, const QVector<FormField> &fields)
{
    for (int i = m_formFieldOverlays.size() - 1; i >= 0; --i) {
        if (m_formFieldOverlays.at(i).page == page)
            m_formFieldOverlays.removeAt(i);
    }
    for (const FormField &field : fields) {
        FormFieldOverlay overlay;
        overlay.page = page;
        overlay.field = field;
        switch (field.type) {
        case FormField::Type::Text:
        case FormField::Type::CheckBox:
        case FormField::Type::RadioButton:
        case FormField::Type::ComboBox:
        case FormField::Type::ListBox:
            m_formFieldOverlays.append(overlay);
            break;
        case FormField::Type::None:
            break;
        }
    }
    viewport()->update();
}

void PageView::clearFormFieldOverlays()
{
    if (m_formFieldOverlays.isEmpty())
        return;
    m_formFieldOverlays.clear();
    viewport()->update();
}

bool PageView::hasPendingChanges() const
{
    if (!m_strokes.isEmpty() || m_drawing || m_erasing || m_imageActive)
        return true;
    if (m_editor) {
        const QString text = m_editor->toPlainText();
        if (m_editingTextAnnotation)
            return text != m_editingTextAnnotationOriginal;
        return !text.trimmed().isEmpty();
    }
    return m_formEditor && formEditorValue() != m_formEditorInitialValue;
}

// mouse

void PageView::mousePressEvent(QMouseEvent *event)
{
    if (m_loading || m_interactionBlocked) {
        event->accept();
        return;
    }

    const QPoint pos = event->position().toPoint();

    if (m_formEditor)
        finishFormEditor(true);

    if (m_editor) {
        finishEditor(true);
        return;
    }

    if (m_redactionTool) {
        if (event->button() == Qt::LeftButton) {
            const int page = pageAtViewportPoint(pos);
            if (page >= 0) {
                m_redactionPage = page;
                m_redactionStart = pagePointFor(page, pos);
                m_redactionPreview = QRectF(m_redactionStart, QSizeF());
                m_redactionDrawing = true;
                viewport()->update();
            }
        }
        event->accept();
        return;
    }

    if (m_imageActive) {
        if (event->button() != Qt::LeftButton)
            return;
        if (m_imageCropping) {
            const int handle = imageCropHandleAt(pos);
            if (handle >= 0) {
                m_imageCropDragging = true;
                switch (handle) {
                case 0: m_imageCropAnchor = m_imageCropRect.bottomRight(); break;
                case 1: m_imageCropAnchor = m_imageCropRect.bottomLeft(); break;
                case 2: m_imageCropAnchor = m_imageCropRect.topRight(); break;
                default: m_imageCropAnchor = m_imageCropRect.topLeft(); break;
                }
            } else {
                m_imageDrag = ImageDrag::Move;
                m_imageGrab = pagePointFor(m_imagePage, pos) - m_imageRect.topLeft();
            }
            return;
        }
        const int handle = imageHandleAt(pos);
        if (handle >= 0) {
            m_imageDrag = ImageDrag::Resize;
            m_imageHandle = handle;
            switch (handle) {
            case 0: m_imageAnchor = m_imageRect.bottomRight(); break;
            case 1: m_imageAnchor = m_imageRect.bottomLeft(); break;
            case 2: m_imageAnchor = m_imageRect.topRight(); break;
            default: m_imageAnchor = m_imageRect.topLeft(); break;
            }
            return;
        }
        if (imagePixelRect().contains(QPointF(pos))) {
            m_imageDrag = ImageDrag::Move;
            m_imageGrab = pagePointFor(m_imagePage, pos) - m_imageRect.topLeft();
            return;
        }
        finishImage(true);
        return;
    }

    if (m_imageAwaiting) {
        if (event->button() != Qt::LeftButton)
            return;
        const int page = pageAtViewportPoint(pos);
        if (page < 0)
            return;

        const QSizeF pageSize = m_pageSizes.at(page);
        const double aspect = double(m_pendingImage.width()) / qMax(1, m_pendingImage.height());
        double width = qMax(30.0, qMin(m_pendingImage.width() * 0.75, pageSize.width() * 0.4));
        double height = width / aspect;
        if (height > pageSize.height() * 0.5) {
            height = pageSize.height() * 0.5;
            width = height * aspect;
        }

        const QPointF center = pagePointFor(page, pos);
        QRectF rect(center.x() - width / 2, center.y() - height / 2, width, height);
        rect.moveLeft(qBound(0.0, rect.left(), qMax(0.0, pageSize.width() - width)));
        rect.moveTop(qBound(0.0, rect.top(), qMax(0.0, pageSize.height() - height)));

        m_imagePage = page;
        m_imageRect = rect;
        m_imageAwaiting = false;
        m_imageActive = true;
        emit imagePlacementStateChanged(true, true);
        emit pendingChangesChanged();
        viewport()->update();
        return;
    }

    if (m_textTool && event->button() == Qt::LeftButton) {
        m_dragging = true;
        m_dragStart = pos;
        m_dragRect = QRect();
        return;
    }
    if (m_drawTool && event->button() == Qt::LeftButton) {
        const int page = pageAtViewportPoint(pos);
        if (m_eraserActive) {
            if (page >= 0) {
                m_erasing = true;
                m_erasePage = page;
                m_eraserCursorPos = pos;
                m_erasePoints.clear();
                const QPointF point = pagePointFor(page, pos);
                m_erasePoints << point;
                m_erasePreviewStrokes.clear();
                if (m_inkStrokeHandler) {
                    QVector<Stroke> strokes;
                    if (m_inkStrokeHandler(page, &strokes)) {
                        m_erasePreviewStrokes.reserve(strokes.size());
                        for (Stroke &stroke : strokes) {
                            ErasePreviewStroke preview;
                            preview.stroke = std::move(stroke);
                            if (!preview.stroke.points.isEmpty()) {
                                qreal minX = preview.stroke.points.first().x();
                                qreal maxX = minX;
                                qreal minY = preview.stroke.points.first().y();
                                qreal maxY = minY;
                                for (const QPointF &strokePoint : preview.stroke.points) {
                                    minX = qMin(minX, strokePoint.x());
                                    maxX = qMax(maxX, strokePoint.x());
                                    minY = qMin(minY, strokePoint.y());
                                    maxY = qMax(maxY, strokePoint.y());
                                }
                                preview.bounds.setCoords(minX, minY, maxX, maxY);
                                const double halfWidth =
                                    preview.stroke.style.widthPoints / 2.0;
                                preview.bounds.adjust(-halfWidth, -halfWidth,
                                                      halfWidth, halfWidth);
                            }
                            m_erasePreviewStrokes.append(std::move(preview));
                        }
                    }
                }
                QRectF changedBounds;
                updateErasePreview(point, point, changedBounds);
                const int radius = qMax(
                    4, qRound(m_drawStyle.widthPoints * m_scale / 2.0)) + 2;
                QRect dirtyRect = QRect(pos.x() - radius, pos.y() - radius,
                                        radius * 2, radius * 2);
                if (!changedBounds.isNull())
                    dirtyRect = dirtyRect.united(
                        erasePreviewViewportRect(changedBounds));
                viewport()->update(dirtyRect);
            }
            return;
        }
        if (page >= 0) {
            m_activeStroke = Stroke();
            m_activeStroke.page = page;
            m_activeStroke.style = m_drawStyle;
            m_activeStroke.points << pagePointFor(page, pos);
            m_drawing = true;
            emit pendingChangesChanged();
        }
        return;
    }

    if (event->button() == Qt::LeftButton && !m_textTool && !m_drawTool) {
        setFocus();
        if (m_selectedImagePage >= 0) {
            const int resizeCorner = imageStampResizeHandleAt(pos);
            if (resizeCorner >= 0) {
                m_resizingImageStamp = true;
                m_imageStampResizeCorner = resizeCorner;
                m_imageStampOriginalBounds = m_selectedImageBounds;
                return;
            }
            if (selectedImageViewportBounds().contains(QPointF(pos))) {
                m_movingImageStamp = true;
                m_imageStampMoveStart = pagePointFor(m_selectedImagePage, pos);
                m_imageStampOriginalBounds = m_selectedImageBounds;
                return;
            }
        }
        if (m_selectedTextAnnotationPage >= 0) {
            const int resizeCorner = textAnnotationResizeHandleAt(pos);
            if (resizeCorner >= 0) {
                m_resizingTextAnnotation = true;
                m_textAnnotationResizeCorner = resizeCorner;
                m_textAnnotationResizeOriginalBounds = m_selectedTextAnnotationBounds;
                return;
            }
            if (selectedTextAnnotationViewportBounds().contains(QPointF(pos))) {
                m_movingTextAnnotation = true;
                m_textAnnotationMoveStart = pagePointFor(
                    m_selectedTextAnnotationPage, pos);
                m_textAnnotationResizeOriginalBounds = m_selectedTextAnnotationBounds;
                return;
            }
        }
        const int page = pageAtViewportPoint(pos);
        int annotationId = 0;
        QRectF annotationBounds;
        QString annotationText;
        TextStyle annotationStyle;
        if (page >= 0 && m_textAnnotationHandler
            && m_textAnnotationHandler(page, pagePointFor(page, pos),
                                       &annotationId, &annotationBounds, &annotationText,
                                       &annotationStyle)) {
            clearSelectedImageStamp();
            selectTextAnnotation(page, annotationId, annotationBounds,
                                 annotationText, annotationStyle);
            clearSelection();
            return;
        }
        clearSelectedTextAnnotation();
        PdfEditor::ImageStamp imageStamp;
        if (page >= 0 && m_imageStampHandler
            && m_imageStampHandler(page, pagePointFor(page, pos), &imageStamp)) {
            selectImageStamp(page, imageStamp);
            m_movingImageStamp = true;
            m_imageStampMoveStart = pagePointFor(page, pos);
            m_imageStampOriginalBounds = imageStamp.bounds;
            clearSelection();
            return;
        }
        clearSelectedImageStamp();
        if (page >= 0 && m_formFieldHandler) {
            FormField field;
            if (m_formFieldHandler(page, pagePointFor(page, pos), &field)) {
                clearSelection();
                if (!field.readOnly
                    && (field.type == FormField::Type::Text
                        || field.type == FormField::Type::ComboBox
                        || field.type == FormField::Type::ListBox))
                    startFormEditor(page, field);
                return;
            }
        }
        const PdfEditor::Link link = page >= 0 ? linkAtViewportPoint(pos)
                                               : PdfEditor::Link();
        if (link.isValid()) {
            m_pressedLinkPage = page;
            m_linkPressPos = pos;
            m_pressedLinkUrl = link.url;
            m_pressedLinkDestinationPage = link.page;
            m_pressedLinkLocation = link.location;
            event->accept();
            return;
        }

        const bool extending = page >= 0 && (event->modifiers() & Qt::ShiftModifier)
                               && !m_selection.isEmpty() && m_selStartPage >= 0;
        if (!extending)
            clearSelection();
        if (page >= 0) {
            m_selecting = true;
            m_selMoved = extending;
            m_selPressPos = pos;
            m_lastMouse = pos;
            if (!extending) {
                m_selStartPage = page;
                m_selStartPoint = pagePointFor(page, pos);
            }
            m_selEndPage = page;
            m_selEndPoint = m_selStartPoint;
            if (extending) {
                m_selEndPoint = pagePointFor(page, pos);
                updateSelection();
                viewport()->update();
            }
            m_autoScroll->start();
        }
        return;
    }
    QAbstractScrollArea::mousePressEvent(event);
}

void PageView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (m_loading || m_interactionBlocked) {
        event->accept();
        return;
    }

    if (!m_textTool && !m_drawTool && event->button() == Qt::LeftButton) {
        const QPoint pos = event->position().toPoint();
        const int page = pageAtViewportPoint(pos);
        int annotationId = 0;
        QRectF bounds;
        QString text;
        TextStyle style;
        if (page >= 0 && m_textAnnotationHandler
            && m_textAnnotationHandler(page, pagePointFor(page, pos),
                                       &annotationId, &bounds, &text, &style)) {
            clearSelectedImageStamp();
            selectTextAnnotation(page, annotationId, bounds, text, style);
            emit textAnnotationEditRequested(page, annotationId, text);
            return;
        }
        if (page >= 0) {
            clearSelectedTextAnnotation();
            PdfEditor::ImageStamp stamp;
            if (m_imageStampHandler
                && m_imageStampHandler(page, pagePointFor(page, pos), &stamp)) {
                selectImageStamp(page, stamp);
                return;
            }
            clearSelectedImageStamp();
            selectWordAt(page, pagePointFor(page, pos));
            return;
        }
    }
    QAbstractScrollArea::mouseDoubleClickEvent(event);
}

void PageView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_loading || m_interactionBlocked)
        return;

    const QPoint pos = event->position().toPoint();
    if (m_redactionTool) {
        if (m_redactionDrawing && m_redactionPage >= 0
            && m_redactionPage < m_pageSizes.size()) {
            QPointF end = pagePointFor(m_redactionPage, pos);
            const QSizeF pageSize = m_pageSizes.at(m_redactionPage);
            end.setX(qBound(0.0, end.x(), pageSize.width()));
            end.setY(qBound(0.0, end.y(), pageSize.height()));
            m_redactionPreview = QRectF(m_redactionStart, end).normalized();
            viewport()->update();
        }
        return;
    }

    const PdfEditor::Link hoveredLink = linkAtViewportPoint(pos);
    setHoveredLink(hoveredLink.url);

    if (m_pressedLinkPage >= 0) {
        if ((pos - m_linkPressPos).manhattanLength() >= QApplication::startDragDistance())
            m_pressedLinkPage = -1;
        else
            return;
    }

    if (m_resizingTextAnnotation) {
        const QPointF point = pagePointFor(m_selectedTextAnnotationPage, pos);
        const QSizeF pageSize = m_pageSizes.at(m_selectedTextAnnotationPage);
        const qreal x = qBound(0.0, point.x(), pageSize.width());
        const qreal y = qBound(0.0, point.y(), pageSize.height());
        QRectF bounds = m_textAnnotationResizeOriginalBounds;
        constexpr qreal minWidth = 20.0;
        constexpr qreal minHeight = 14.0;
        switch (m_textAnnotationResizeCorner) {
        case 0:
            bounds.setLeft(qMin(x, bounds.right() - minWidth));
            bounds.setTop(qMin(y, bounds.bottom() - minHeight));
            break;
        case 1:
            bounds.setRight(qMax(x, bounds.left() + minWidth));
            bounds.setTop(qMin(y, bounds.bottom() - minHeight));
            break;
        case 2:
            bounds.setLeft(qMin(x, bounds.right() - minWidth));
            bounds.setBottom(qMax(y, bounds.top() + minHeight));
            break;
        case 3:
            bounds.setRight(qMax(x, bounds.left() + minWidth));
            bounds.setBottom(qMax(y, bounds.top() + minHeight));
            break;
        default:
            break;
        }
        m_selectedTextAnnotationBounds = bounds;
        viewport()->update();
        return;
    }

    if (m_movingTextAnnotation) {
        const QPointF point = pagePointFor(m_selectedTextAnnotationPage, pos);
        const QSizeF pageSize = m_pageSizes.at(m_selectedTextAnnotationPage);
        const QPointF delta = point - m_textAnnotationMoveStart;
        QRectF bounds = m_textAnnotationResizeOriginalBounds;
        const qreal x = qBound(0.0, bounds.left() + delta.x(),
                               qMax(0.0, pageSize.width() - bounds.width()));
        const qreal y = qBound(0.0, bounds.top() + delta.y(),
                               qMax(0.0, pageSize.height() - bounds.height()));
        bounds.moveTopLeft(QPointF(x, y));
        m_selectedTextAnnotationBounds = bounds;
        viewport()->update();
        return;
    }

    if (m_resizingImageStamp) {
        const QPointF point = pagePointFor(m_selectedImagePage, pos);
        const QSizeF pageSize = m_pageSizes.at(m_selectedImagePage);
        const QRectF original = m_imageStampOriginalBounds;
        const bool right = m_imageStampResizeCorner == 1
                           || m_imageStampResizeCorner == 3;
        const bool down = m_imageStampResizeCorner == 2
                          || m_imageStampResizeCorner == 3;
        const QPointF anchor(
            right ? original.left() : original.right(),
            down ? original.top() : original.bottom());
        const qreal aspect = original.width() / original.height();
        const qreal signX = right ? 1.0 : -1.0;
        const qreal signY = down ? 1.0 : -1.0;
        const qreal availableWidth = right ? pageSize.width() - anchor.x() : anchor.x();
        const qreal availableHeight = down ? pageSize.height() - anchor.y() : anchor.y();
        const qreal maxWidth = qMin(availableWidth, availableHeight * aspect);
        qreal width = qMax(signX * (point.x() - anchor.x()),
                           signY * (point.y() - anchor.y()) * aspect);
        width = qBound(qMin(12.0, maxWidth), width, maxWidth);
        const qreal height = width / aspect;
        m_selectedImageBounds = QRectF(
            right ? anchor.x() : anchor.x() - width,
            down ? anchor.y() : anchor.y() - height, width, height);
        viewport()->update();
        return;
    }

    if (m_movingImageStamp) {
        const QPointF point = pagePointFor(m_selectedImagePage, pos);
        const QSizeF pageSize = m_pageSizes.at(m_selectedImagePage);
        const QPointF delta = point - m_imageStampMoveStart;
        QRectF bounds = m_imageStampOriginalBounds;
        const qreal x = qBound(0.0, bounds.left() + delta.x(),
                               qMax(0.0, pageSize.width() - bounds.width()));
        const qreal y = qBound(0.0, bounds.top() + delta.y(),
                               qMax(0.0, pageSize.height() - bounds.height()));
        bounds.moveTopLeft(QPointF(x, y));
        m_selectedImageBounds = bounds;
        viewport()->update();
        return;
    }

    if (m_imageActive) {
        if (m_imageCropping) {
            if (m_imageCropDragging)
                updateImageCropRect(pos);
            else {
                const int handle = imageCropHandleAt(pos);
                if (handle >= 0)
                    viewport()->setCursor((handle == 0 || handle == 3)
                                              ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor);
                else if (imagePixelRect().contains(QPointF(pos)))
                    viewport()->setCursor(Qt::SizeAllCursor);
                else
                    viewport()->setCursor(Qt::ArrowCursor);
            }
            return;
        }
        if (m_imageDrag == ImageDrag::None) {
            updateImageCursor(pos);
            return;
        }

        const QPointF p = pagePointFor(m_imagePage, pos);
        const QSizeF pageSize = m_pageSizes.at(m_imagePage);

        if (m_imageDrag == ImageDrag::Move) {
            QPointF topLeft = p - m_imageGrab;
            topLeft.setX(qBound(0.0, topLeft.x(),
                                qMax(0.0, pageSize.width() - m_imageRect.width())));
            topLeft.setY(qBound(0.0, topLeft.y(),
                                qMax(0.0, pageSize.height() - m_imageRect.height())));
            m_imageRect.moveTopLeft(topLeft);
        } else {
            const double aspect = double(m_pendingImage.width())
                                  / qMax(1, m_pendingImage.height());
            const bool right = (m_imageHandle == 1 || m_imageHandle == 3);
            const bool down = (m_imageHandle == 2 || m_imageHandle == 3);

            double width = right ? p.x() - m_imageAnchor.x() : m_imageAnchor.x() - p.x();
            const double roomX = right ? pageSize.width() - m_imageAnchor.x() : m_imageAnchor.x();
            const double roomY = down ? pageSize.height() - m_imageAnchor.y() : m_imageAnchor.y();
            const double maxWidth = qMin(roomX, roomY * aspect);
            width = qBound(10.0, width, qMax(10.0, maxWidth));
            const double height = width / aspect;

            m_imageRect = QRectF(right ? m_imageAnchor.x() : m_imageAnchor.x() - width,
                                 down ? m_imageAnchor.y() : m_imageAnchor.y() - height,
                                 width, height);
        }
        viewport()->update();
        return;
    }

    if (m_drawTool && m_eraserActive) {
        const QPoint previousCursorPos = m_eraserCursorPos;
        m_eraserCursorPos = pos;
        const int radius = qMax(
            4, qRound(m_drawStyle.widthPoints * m_scale / 2.0)) + 2;
        QRect dirtyRect(previousCursorPos.x() - radius,
                        previousCursorPos.y() - radius,
                        radius * 2, radius * 2);
        dirtyRect = dirtyRect.united(
            QRect(pos.x() - radius, pos.y() - radius, radius * 2, radius * 2));
        if (m_erasing) {
            const QPointF point = pagePointFor(m_erasePage, pos);
            const QPointF previous = m_erasePoints.last();
            if (point != previous) {
                m_erasePoints << point;
                QRectF changedBounds;
                updateErasePreview(previous, point, changedBounds);
                if (!changedBounds.isNull())
                    dirtyRect = dirtyRect.united(
                        erasePreviewViewportRect(changedBounds));
            }
        } else {
            m_erasePage = pageAtViewportPoint(pos);
        }
        viewport()->update(dirtyRect);
        return;
    }

    if (m_drawing) {
        const QPointF point = pagePointFor(m_activeStroke.page, pos);
        if (QLineF(m_activeStroke.points.last(), point).length() * m_scale >= 1.5)
            m_activeStroke.points << point;
        viewport()->update();
        return;
    }
    if (m_dragging) {
        m_dragRect = QRect(m_dragStart, pos).normalized();
        viewport()->update();
        return;
    }

    if (m_selecting) {
        m_lastMouse = pos;
        extendSelection(pos);
        return;
    }

    if (!m_textTool && !m_drawTool && !m_imageAwaiting && !m_editor) {
        const int imageHandle = imageStampResizeHandleAt(pos);
        if (imageHandle >= 0) {
            viewport()->setCursor((imageHandle == 0 || imageHandle == 3)
                                      ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor);
        } else if (selectedImageViewportBounds().contains(QPointF(pos))) {
            viewport()->setCursor(Qt::SizeAllCursor);
        } else {
            const int handle = textAnnotationResizeHandleAt(pos);
            if (handle >= 0) {
                viewport()->setCursor((handle == 0 || handle == 3)
                                          ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor);
            } else if (selectedTextAnnotationViewportBounds().contains(QPointF(pos))) {
                viewport()->setCursor(Qt::SizeAllCursor);
            } else {
                viewport()->setCursor(hoveredLink.isValid()
                                          ? Qt::PointingHandCursor
                                          : pageAtViewportPoint(pos) >= 0
                                                ? Qt::IBeamCursor : Qt::ArrowCursor);
            }
        }
    }
    QAbstractScrollArea::mouseMoveEvent(event);
}

void PageView::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_loading || m_interactionBlocked) {
        event->accept();
        return;
    }

    if (m_redactionTool) {
        if (m_redactionDrawing && event->button() == Qt::LeftButton
            && m_redactionPage >= 0 && m_redactionPage < m_pageSizes.size()) {
            QPointF end = pagePointFor(
                m_redactionPage, event->position().toPoint());
            const QSizeF pageSize = m_pageSizes.at(m_redactionPage);
            end.setX(qBound(0.0, end.x(), pageSize.width()));
            end.setY(qBound(0.0, end.y(), pageSize.height()));
            const QRectF bounds = QRectF(m_redactionStart, end).normalized();
            if (bounds.width() >= 6.0 && bounds.height() >= 6.0) {
                m_redactionAreas.append({ m_redactionPage, bounds });
                emit redactionAreasChanged(m_redactionAreas.size());
            }
            m_redactionDrawing = false;
            m_redactionPage = -1;
            m_redactionPreview = QRectF();
            viewport()->update();
        }
        event->accept();
        return;
    }

    if (m_movingTextAnnotation && event->button() == Qt::LeftButton) {
        m_movingTextAnnotation = false;
        viewport()->setCursor(Qt::ArrowCursor);
        if (m_selectedTextAnnotationBounds != m_textAnnotationResizeOriginalBounds) {
            emit textAnnotationBoundsChanged(m_selectedTextAnnotationPage,
                                             m_selectedTextAnnotationId,
                                             m_selectedTextAnnotationContents,
                                             m_textAnnotationResizeOriginalBounds,
                                             m_selectedTextAnnotationBounds,
                                             m_selectedTextAnnotationStyle);
        }
        return;
    }

    if (m_movingImageStamp && event->button() == Qt::LeftButton) {
        m_movingImageStamp = false;
        viewport()->setCursor(Qt::ArrowCursor);
        if (m_selectedImageBounds != m_imageStampOriginalBounds) {
            m_pendingImageStampPreview = {
                m_selectedImagePage, m_imageStampOriginalBounds,
                m_selectedImageBounds, m_selectedImagePixmap
            };
            emit imageStampBoundsChanged(m_selectedImagePage, m_selectedImageId,
                                         m_imageStampOriginalBounds,
                                         m_selectedImageBounds);
        }
        return;
    }

    if (m_resizingImageStamp && event->button() == Qt::LeftButton) {
        m_resizingImageStamp = false;
        m_imageStampResizeCorner = -1;
        viewport()->setCursor(Qt::ArrowCursor);
        if (m_selectedImageBounds != m_imageStampOriginalBounds) {
            m_pendingImageStampPreview = {
                m_selectedImagePage, m_imageStampOriginalBounds,
                m_selectedImageBounds, m_selectedImagePixmap
            };
            emit imageStampBoundsChanged(m_selectedImagePage, m_selectedImageId,
                                         m_imageStampOriginalBounds,
                                         m_selectedImageBounds);
        }
        return;
    }

    if (m_resizingTextAnnotation && event->button() == Qt::LeftButton) {
        m_resizingTextAnnotation = false;
        m_textAnnotationResizeCorner = -1;
        viewport()->setCursor(Qt::ArrowCursor);
        if (m_selectedTextAnnotationBounds != m_textAnnotationResizeOriginalBounds) {
            emit textAnnotationBoundsChanged(m_selectedTextAnnotationPage,
                                             m_selectedTextAnnotationId,
                                             m_selectedTextAnnotationContents,
                                             m_textAnnotationResizeOriginalBounds,
                                             m_selectedTextAnnotationBounds,
                                             m_selectedTextAnnotationStyle);
        }
        return;
    }

    if (m_erasing && event->button() == Qt::LeftButton) {
        const int page = m_erasePage;
        const QVector<QPointF> path = m_erasePoints;
        const double radius = m_drawStyle.widthPoints / 2.0;
        QVector<Stroke> erasedStrokes;
        for (const ErasePreviewStroke &preview : std::as_const(m_erasePreviewStrokes)) {
            if (preview.erased)
                erasedStrokes.append(preview.stroke);
        }
        preserveErasedStrokes(page, erasedStrokes);
        m_erasing = false;
        m_erasePoints.clear();
        m_erasePreviewStrokes.clear();
        viewport()->update();
        if (!path.isEmpty())
            emit eraseRequested(page, path, radius);
        emit pendingChangesChanged();
        return;
    }

    if (m_selecting && event->button() == Qt::LeftButton) {
        m_selecting = false;
        m_autoScroll->stop();
        if (!m_selection.isEmpty())
            emit textSelected();
        return;
    }

    if (m_imageDrag != ImageDrag::None) {
        m_imageDrag = ImageDrag::None;
        viewport()->update();
        return;
    }

    if (m_imageCropping && m_imageCropDragging && event->button() == Qt::LeftButton) {
        updateImageCropRect(event->position().toPoint());
        m_imageCropDragging = false;
        applyImageCrop();
        return;
    }

    if (m_drawing && event->button() == Qt::LeftButton) {
        m_drawing = false;
        if (m_activeStroke.points.size() == 1)
            m_activeStroke.points << m_activeStroke.points.first() + QPointF(0.1, 0.0);
        m_strokes.append(m_activeStroke);
        m_activeStroke = Stroke();
        viewport()->update();
        emit pendingStrokesChanged();
        emit pendingChangesChanged();
        return;
    }

    if (!m_dragging || event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mouseReleaseEvent(event);
        return;
    }
    m_dragging = false;

    QRect box = QRect(m_dragStart, event->position().toPoint()).normalized();
    m_dragRect = QRect();
    viewport()->update();

    if (box.width() < 12 || box.height() < 12)
        box = QRect(m_dragStart, QSize(int(200 * m_scale), int(60 * m_scale)));

    const int page = pageAtViewportPoint(m_dragStart);
    if (page < 0)
        return;

    const QRect pageRect = pageRectInViewport(page);
    box = box.intersected(pageRect);
    if (box.width() < 12 || box.height() < 12)
        return;

    startEditor(page, QRectF((box.x() - pageRect.x()) / m_scale,
                             (box.y() - pageRect.y()) / m_scale,
                             box.width() / m_scale, box.height() / m_scale));
}

// text editor

void PageView::startEditor(int page, const QRectF &boxPoints)
{
    m_editorPage = page;
    m_editorBox = boxPoints;
    m_editorStyle = m_style;
    m_editingTextAnnotation = false;
    m_editingTextAnnotationId = 0;
    m_editingTextAnnotationOriginal.clear();

    m_editor = new QPlainTextEdit(viewport());
    m_editor->setFrameShape(QFrame::NoFrame);
    m_editor->installEventFilter(this);
    connect(m_editor, &QPlainTextEdit::textChanged,
            this, &PageView::pendingChangesChanged);
    applyEditorStyle();
    positionEditor();
    m_editor->show();
    m_editor->setFocus();
    emit pendingChangesChanged();
}

void PageView::startAnnotationEditor(int page, int annotationId,
                                     const QRectF &boxPoints, const QString &text,
                                     const TextStyle &style)
{
    finishEditor(false);
    m_textTool = false;
    startEditor(page, boxPoints);
    m_editingTextAnnotation = true;
    m_editingTextAnnotationId = annotationId;
    m_editingTextAnnotationOriginal = text;
    m_editingTextAnnotationStyleChanged = false;
    m_editorStyle = style;
    m_editor->setPlainText(text);
    m_editor->moveCursor(QTextCursor::End);
    applyEditorStyle();
    m_editor->setFocus();
    emit textAnnotationEditingStyleChanged(style);
}

void PageView::applyEditorStyle()
{
    if (!m_editor)
        return;

    const QString background = m_editorStyle.color.lightness() > 200
        ? QStringLiteral("rgba(60,60,60,225)")
        : QStringLiteral("rgba(255,255,255,215)");

    m_editor->setStyleSheet(
        QStringLiteral("QPlainTextEdit { color: %1; background: %2; border: 2px solid #0066cc;"
                       " font-family: \"%3\"; font-size: %4px; }")
            .arg(m_editorStyle.color.name(), background, m_editorStyle.family)
            .arg(qMax(4, int(m_editorStyle.sizePoints * m_scale))));
}

void PageView::positionEditor()
{
    if (!m_editor || m_editorPage < 0 || m_editorPage >= m_pageRects.size())
        return;
    const QRect page = pageRectInViewport(m_editorPage);
    m_editor->setGeometry(page.x() + int(m_editorBox.x() * m_scale),
                          page.y() + int(m_editorBox.y() * m_scale),
                          qMax(40, int(m_editorBox.width() * m_scale)),
                          qMax(24, int(m_editorBox.height() * m_scale)));
}

void PageView::finishEditor(bool commit)
{
    if (!m_editor)
        return;

    QPlainTextEdit *editor = m_editor;
    m_editor = nullptr;

    const QString text = editor->toPlainText();
    const int page = m_editorPage;
    const QRectF box = m_editorBox;
    const TextStyle style = m_editorStyle;
    const bool editingAnnotation = m_editingTextAnnotation;
    const int annotationId = m_editingTextAnnotationId;
    const QString originalText = m_editingTextAnnotationOriginal;
    const bool annotationStyleChanged = m_editingTextAnnotationStyleChanged;
    m_editingTextAnnotation = false;
    m_editingTextAnnotationId = 0;
    m_editingTextAnnotationOriginal.clear();
    m_editingTextAnnotationStyleChanged = false;

    editor->hide();
    editor->deleteLater();

    if (!editingAnnotation) {
        m_textTool = false;
        viewport()->setCursor(Qt::ArrowCursor);
        setFocus();
        emit textToolFinished();
    }

    if (commit) {
        if (editingAnnotation) {
            if (text != originalText || annotationStyleChanged)
                emit textAnnotationEdited(page, annotationId, originalText, text, style);
        } else if (!text.trimmed().isEmpty()) {
            emit textPlaced(page, box, text.trimmed(), style);
        }
    }
    emit pendingChangesChanged();
}

void PageView::startFormEditor(int page, const FormField &field)
{
    finishFormEditor(false);
    m_activeFormField = field;
    m_formEditorPage = page;
    m_formEditorInitialValue = field.value;

    if (field.type == FormField::Type::Text && field.multiline) {
        m_formTextEditor = new QPlainTextEdit(viewport());
        m_formTextEditor->setPlainText(field.value);
        m_formTextEditor->installEventFilter(this);
        connect(m_formTextEditor, &QPlainTextEdit::textChanged,
                this, &PageView::pendingChangesChanged);
        m_formEditor = m_formTextEditor;
    } else if (field.type == FormField::Type::Text) {
        m_formLineEditor = new QLineEdit(viewport());
        m_formLineEditor->setText(field.value);
        m_formLineEditor->setEchoMode(field.password ? QLineEdit::Password
                                                     : QLineEdit::Normal);
        m_formLineEditor->installEventFilter(this);
        connect(m_formLineEditor, &QLineEdit::textChanged,
                this, &PageView::pendingChangesChanged);
        m_formEditor = m_formLineEditor;
    } else {
        m_formChoiceEditor = new QComboBox(viewport());
        m_formChoiceEditor->setEditable(field.editableChoice);
        for (int i = 0; i < field.options.size(); ++i) {
            const QString optionValue = i < field.optionValues.size()
                ? field.optionValues.at(i) : field.options.at(i);
            m_formChoiceEditor->addItem(field.options.at(i), optionValue);
        }
        int current = field.optionValues.indexOf(field.value);
        if (current < 0)
            current = field.options.indexOf(field.value);
        if (current < 0 && !field.editableChoice) {
            m_formChoiceEditor->insertItem(0, field.value, field.value);
            current = 0;
        }
        if (current >= 0) {
            m_formChoiceEditor->setCurrentIndex(current);
        } else if (field.editableChoice) {
            m_formChoiceEditor->setEditText(field.value);
        }
        m_formChoiceEditor->installEventFilter(this);
        connect(m_formChoiceEditor, &QComboBox::currentTextChanged,
                this, &PageView::pendingChangesChanged);
        if (m_formChoiceEditor->lineEdit())
            m_formChoiceEditor->lineEdit()->installEventFilter(this);
        m_formEditor = m_formChoiceEditor;
    }

    m_formEditor->setStyleSheet(
        QStringLiteral("QLineEdit, QPlainTextEdit, QComboBox {"
                       " color: #111; background: rgba(255,255,255,245);"
                       " border: 2px solid #0066cc; padding: 1px; }"));
    positionFormEditor();
    m_formEditor->show();
    m_formEditor->setFocus();
    if (m_formLineEditor)
        m_formLineEditor->selectAll();
    emit pendingChangesChanged();
}

void PageView::positionFormEditor()
{
    if (!m_formEditor || m_formEditorPage < 0 || m_formEditorPage >= m_pageRects.size())
        return;
    const QRect page = pageRectInViewport(m_formEditorPage);
    const QRect bounds(
        page.x() + qRound(m_activeFormField.bounds.x() * m_scale),
        page.y() + qRound(m_activeFormField.bounds.y() * m_scale),
        qMax(1, qRound(m_activeFormField.bounds.width() * m_scale)),
        qMax(1, qRound(m_activeFormField.bounds.height() * m_scale)));
    m_formEditor->setGeometry(bounds);
}

void PageView::finishFormEditor(bool commit)
{
    if (!m_formEditor)
        return;

    const int page = m_formEditorPage;
    const FormField field = m_activeFormField;
    const QString initialValue = m_formEditorInitialValue;
    const QString value = formEditorValue();

    QWidget *editor = m_formEditor;
    m_formEditor = nullptr;
    m_formLineEditor = nullptr;
    m_formTextEditor = nullptr;
    m_formChoiceEditor = nullptr;
    m_formEditorPage = -1;
    m_activeFormField = FormField();
    editor->hide();
    editor->deleteLater();
    setFocus();
    m_formEditorInitialValue.clear();

    if (commit && value != initialValue && m_formFieldValueHandler) {
        if (!m_formFieldValueHandler(page, field.widgetIndex, value)) {
            FormField retryField = field;
            retryField.value = value;
            if (page >= 0 && page < m_pageRects.size())
                startFormEditor(page, retryField);
            return;
        }

        FormFieldOverlay overlay;
        overlay.page = page;
        overlay.field = field;
        overlay.field.value = value;
        int existing = -1;
        for (int i = 0; i < m_formFieldOverlays.size(); ++i) {
            if (m_formFieldOverlays.at(i).page == page
                && m_formFieldOverlays.at(i).field.widgetIndex == field.widgetIndex) {
                existing = i;
                break;
            }
        }
        if (existing >= 0)
            m_formFieldOverlays[existing] = overlay;
        else
            m_formFieldOverlays.append(overlay);
        viewport()->update();
    }
    emit pendingChangesChanged();
}

QString PageView::formEditorValue() const
{
    if (m_formLineEditor)
        return m_formLineEditor->text();
    if (m_formTextEditor)
        return m_formTextEditor->toPlainText();
    if (!m_formChoiceEditor)
        return QString();

    const int matchingOption = m_formChoiceEditor->findText(
        m_formChoiceEditor->currentText());
    if (m_formChoiceEditor->isEditable() && matchingOption < 0)
        return m_formChoiceEditor->currentText();
    return m_formChoiceEditor->itemData(
        matchingOption >= 0 ? matchingOption
                            : m_formChoiceEditor->currentIndex()).toString();
}

bool PageView::eventFilter(QObject *watched, QEvent *event)
{
    if (m_interactionBlocked
        && (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease
            || event->type() == QEvent::MouseButtonPress
            || event->type() == QEvent::MouseButtonRelease
            || event->type() == QEvent::MouseButtonDblClick))
        return true;

    if (watched == viewport() && event->type() == QEvent::Leave)
        setHoveredLink(QUrl());

    if (m_formEditor
        && (watched == m_formEditor
            || (m_formChoiceEditor && watched == m_formChoiceEditor->lineEdit()))) {
        if (event->type() == QEvent::KeyPress) {
            auto *key = static_cast<QKeyEvent *>(event);
            if (key->key() == Qt::Key_Escape) {
                finishFormEditor(false);
                return true;
            }
            const bool enter = key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter;
            const bool multiline = m_formTextEditor != nullptr;
            if (enter && (!multiline || (key->modifiers() & Qt::ControlModifier))) {
                finishFormEditor(true);
                return true;
            }
        } else if (event->type() == QEvent::FocusOut) {
            const Qt::FocusReason reason = static_cast<QFocusEvent *>(event)->reason();
            if (reason != Qt::ActiveWindowFocusReason && reason != Qt::PopupFocusReason) {
                QTimer::singleShot(0, this, [this]() {
                    if (!m_formEditor)
                        return;
                    QWidget *focus = QApplication::focusWidget();
                    if ((focus && (focus == m_formEditor || m_formEditor->isAncestorOf(focus)))
                        || QApplication::activePopupWidget())
                        return;
                    finishFormEditor(true);
                });
            }
        }
    }

    if (m_editor && watched == m_editor) {
        if (event->type() == QEvent::KeyPress) {
            auto *key = static_cast<QKeyEvent *>(event);
            if (key->key() == Qt::Key_Escape) {
                finishEditor(false);
                return true;
            }
            if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
                && (key->modifiers() & Qt::ControlModifier)) {
                finishEditor(true);
                return true;
            }
        } else if (event->type() == QEvent::FocusOut) {
            const Qt::FocusReason reason = static_cast<QFocusEvent *>(event)->reason();
            if (reason != Qt::ActiveWindowFocusReason && reason != Qt::PopupFocusReason) {
                QTimer::singleShot(0, this, [this]() {
                    if (!m_editor)
                        return;
                    QWidget *focus = QApplication::focusWidget();
                    const auto isTextStyleControl = [](QWidget *widget) {
                        for (; widget; widget = widget->parentWidget()) {
                            if (widget->property("textEditorStyleControl").toBool())
                                return true;
                        }
                        return false;
                    };
                    if (isTextStyleControl(focus)
                        || isTextStyleControl(QApplication::widgetAt(QCursor::pos())))
                        return;
                    if (QApplication::activePopupWidget())
                        return;
                    finishEditor(true);
                });
            }
        }
    }
    return QAbstractScrollArea::eventFilter(watched, event);
}