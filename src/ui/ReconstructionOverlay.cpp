#include "ReconstructionOverlay.h"
#include "PageView.h"

#include "reconstruction/AutoReconstruct.h"
#include "reconstruction/FontClassifier.h"
#include "reconstruction/RichText.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QCursor>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextFragment>
#include <QTextLayout>
#include <QTextOption>
#include <QTimer>
#include <QWidget>

#include <utility>

namespace {

bool isStyleControl(QWidget *widget)
{
    for (; widget; widget = widget->parentWidget()) {
        if (widget->property("textEditorStyleControl").toBool())
            return true;
    }
    return false;
}

bool sameBounds(const QRectF &a, const QRectF &b)
{
    return qAbs(a.left() - b.left()) < 0.5 && qAbs(a.top() - b.top()) < 0.5
        && qAbs(a.right() - b.right()) < 0.5 && qAbs(a.bottom() - b.bottom()) < 0.5;
}

class MoveHandle : public QWidget
{
public:
    explicit MoveHandle(QWidget *parent)
        : QWidget(parent)
    {
        setFixedSize(34, 14);
        setCursor(Qt::SizeAllCursor);
        setToolTip(QStringLiteral("Drag to move this text (Alt+arrow keys nudge it)"));
        setProperty("textEditorStyleControl", true);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), QColor(224, 138, 0));
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        for (int column = 0; column < 3; ++column) {
            for (int row = 0; row < 2; ++row)
                painter.drawEllipse(QPointF(9.0 + column * 8.0, 4.5 + row * 5.0), 1.5, 1.5);
        }
    }
};

class ResizeHandle : public QWidget
{
public:
    ResizeHandle(QWidget *parent, int corner)
        : QWidget(parent)
    {
        setFixedSize(10, 10);
        setCursor((corner == 0 || corner == 3) ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor);
        setToolTip(QStringLiteral("Drag to resize this text box"));
        setProperty("textEditorStyleControl", true);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::white);
        painter.setPen(QPen(QColor(224, 138, 0), 1));
        painter.drawRect(rect().adjusted(0, 0, -1, -1));
    }
};

class DeleteHandle : public QWidget
{
public:
    explicit DeleteHandle(QWidget *parent)
        : QWidget(parent)
    {
        setFixedSize(22, 14);
        setCursor(Qt::PointingHandCursor);
        setToolTip(QStringLiteral("Delete this text box (Alt+Delete)"));
        setProperty("textEditorStyleControl", true);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), QColor(200, 40, 40));
        painter.setPen(QPen(Qt::white, 1.8, Qt::SolidLine, Qt::RoundCap));
        const QPointF center = QRectF(rect()).center();
        painter.drawLine(QPointF(center.x() - 3.5, center.y() - 3.5),
                         QPointF(center.x() + 3.5, center.y() + 3.5));
        painter.drawLine(QPointF(center.x() - 3.5, center.y() + 3.5),
                         QPointF(center.x() + 3.5, center.y() - 3.5));
    }
};

Reconstruction::TextRun newTextRun()
{
    Reconstruction::TextRun run;
    run.fontSize = 12.0f;
    run.family = Reconstruction::FontFamily::Sans;
    run.color = QColor(Qt::black);
    return run;
}

Reconstruction::TextRun styleFromFormat(const QTextCharFormat &format, double pixelsPerPoint,
                                        const Reconstruction::TextRun &fallback)
{
    Reconstruction::TextRun run = fallback;
    run.text.clear();
    run.fontName = format.property(Reconstruction::kFontNameProperty).toString();

    double size = format.property(Reconstruction::kFontSizeProperty).toDouble();
    if (size <= 0.0 && pixelsPerPoint > 0.0 && format.font().pixelSize() > 0)
        size = format.font().pixelSize() / pixelsPerPoint;
    if (size > 0.0)
        run.fontSize = float(size);

    const int kind = format.property(Reconstruction::kFontFamilyProperty).toInt();
    if (kind > int(Reconstruction::FontFamily::Unknown)
        && kind <= int(Reconstruction::FontFamily::Mono))
        run.family = Reconstruction::FontFamily(kind);

    run.bold = format.font().bold();
    run.italic = format.font().italic();
    run.underlined = format.fontUnderline();
    if (format.foreground().style() != Qt::NoBrush)
        run.color = format.foreground().color();
    return run;
}

bool isDeletion(const ReconstructionEdit &edit)
{
    return !edit.isNew && edit.paragraph.text.trimmed().isEmpty();
}

void fitNewTextMetrics(Reconstruction::TextParagraph &paragraph, double fallbackSize)
{
    double biggest = 0.0;
    for (const Reconstruction::TextRun &run : std::as_const(paragraph.runs))
        biggest = qMax(biggest, double(run.fontSize));
    if (biggest <= 0.0)
        biggest = fallbackSize;
    if (biggest <= 0.0)
        return;

    paragraph.fontSize = float(biggest);
    paragraph.lineSpacing = biggest * 1.2;
}

}

ReconstructionOverlay::ReconstructionOverlay(PageView *view)
    : QObject(view)
    , m_view(view)
    , m_newTextStyle(newTextRun())
{
}

void ReconstructionOverlay::setActive(bool active)
{
    if (m_active == active)
        return;

    if (!active) {
        flush();
        setAddTextMode(false);
    }

    m_active = active;
    m_result.reset();
    m_page = -1;
    m_hover = -1;
    m_hoverPending = -1;
    m_view->viewport()->setCursor(Qt::ArrowCursor);
    m_view->viewport()->update();
}

void ReconstructionOverlay::setResult(
    int page, std::shared_ptr<const Reconstruction::ReconstructedPage> result, int source)
{
    if (!m_active)
        return;

    m_result = std::move(result);
    m_page = page;
    m_source = source;
    m_hover = -1;
    m_hoverPending = -1;
    m_view->viewport()->update();
}

void ReconstructionOverlay::reset()
{
    cancelEdit();
    setAddTextMode(false);
    m_result.reset();
    m_page = -1;
    m_hover = -1;
    m_hoverPending = -1;
    if (!m_pending.isEmpty()) {
        m_pending.clear();
        emit pendingChanged(0);
    }
    m_view->viewport()->update();
}

// drawing and mouse

void ReconstructionOverlay::paint(QPainter &painter, int page, const QRect &target)
{
    if (!m_active)
        return;

    if (m_editor && m_editPage == page && !m_newText) {
        painter.fillRect(m_view->pageToViewport(target, m_edited.bounds),
                         m_edited.backgroundColor);
    }

    drawPending(painter, page, target);

    if (m_addDragging && m_addPage == page) {
        painter.setPen(QPen(QColor(224, 138, 0), 1.5, Qt::DashLine));
        painter.setBrush(QColor(224, 138, 0, 35));
        painter.drawRect(m_view->pageToViewport(target, QRectF(m_addStart, m_addEnd).normalized()));
    }

    if (!m_result || m_page != page)
        return;

    for (int i = 0; i < int(m_result->paragraphs.size()); ++i) {
        const QRectF &bounds = m_result->paragraphs.at(i).bounds;

        if (isStaged(page, bounds)
            || (m_editor && m_editPage == page && sameBounds(bounds, m_edited.bounds)))
            continue;

        const QRectF box = m_view->pageToViewport(target, bounds);
        const bool hovered = (i == m_hover);

        painter.fillRect(box, hovered ? QColor(255, 160, 0, 70) : QColor(40, 120, 255, 28));
        painter.setPen(QPen(hovered ? QColor(255, 140, 0) : QColor(40, 120, 255, 200),
                            hovered ? 2.5 : 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(box);
    }
}

void ReconstructionOverlay::drawPending(QPainter &painter, int page, const QRect &target)
{
    const double scale = m_view->m_scale;

    for (int i = 0; i < int(m_pending.size()); ++i) {
        PendingReconstructionEdit &pending = m_pending[i];
        const ReconstructionEdit &edit = pending.edit;
        if (edit.page != page)
            continue;

        if (m_editor && m_editPage == page && sameBounds(edit.paragraph.bounds, m_edited.bounds))
            continue;

        if (!edit.isNew)
            painter.fillRect(m_view->pageToViewport(target, edit.paragraph.bounds),
                             edit.paragraph.backgroundColor);

        if (isDeletion(edit))
            continue;

        QTextDocument *document = previewDocument(pending, scale);
        const QRectF region = m_view->pageToViewport(target, edit.region);
        const double top = target.y() + edit.firstBaselineY * scale
                           - Reconstruction::firstBaselineOffset(document);
        painter.save();
        painter.translate(region.x(), top);
        document->drawContents(&painter);
        painter.restore();

        const bool hovered = (i == m_hoverPending);
        const QRectF box = m_view->pageToViewport(target, pending.box);
        if (hovered)
            painter.fillRect(box, QColor(255, 160, 0, 40));
        painter.setPen(QPen(QColor(224, 138, 0), hovered ? 2.5 : 1.5, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(box);
    }
}

QTextDocument *ReconstructionOverlay::previewDocument(PendingReconstructionEdit &pending,
                                                      double scale)
{
    if (!pending.preview || !qFuzzyCompare(pending.previewScale, scale)) {
        pending.preview.reset(Reconstruction::makeDocument(
                                  pending.edit.paragraph, scale,
                                  pending.edit.region.width() * scale).release());
        pending.previewScale = scale;
    }
    return pending.preview.get();
}

bool ReconstructionOverlay::handleMousePress(QMouseEvent *event)
{
    if (!m_active)
        return false;
    if (event->button() != Qt::LeftButton)
        return false;

    if (m_editor)
        commitEdit();

    const QPoint pos = event->position().toPoint();
    const int page = m_view->pageAtViewportPoint(pos);
    if (page < 0)
        return true;

    const QPointF point = m_view->pagePointFor(page, pos);

    if (m_addMode) {
        m_addPage = page;
        m_addStart = point;
        m_addEnd = point;
        m_addDragging = true;
        m_view->viewport()->update();
        return true;
    }

    const int pendingIndex = pendingIndexAt(page, point);
    if (pendingIndex >= 0) {
        startEditPending(pendingIndex);
        return true;
    }

    if (m_result && page == m_page) {
        const int index = paragraphAt(point);
        if (index >= 0)
            startEdit(index);
    }
    return true;
}

bool ReconstructionOverlay::handleMouseMove(const QPoint &pos)
{
    if (!m_active)
        return false;

    if (m_addDragging) {
        m_addEnd = m_view->pagePointFor(m_addPage, pos);
        m_view->viewport()->update();
        return true;
    }

    int hover = -1;
    int hoverPending = -1;
    if (!m_editor) {
        const int page = m_view->pageAtViewportPoint(pos);
        if (page >= 0) {
            const QPointF point = m_view->pagePointFor(page, pos);
            hoverPending = pendingIndexAt(page, point);
            if (hoverPending < 0 && m_result && page == m_page)
                hover = paragraphAt(point);
        }
    }

    if (hover != m_hover || hoverPending != m_hoverPending) {
        m_hover = hover;
        m_hoverPending = hoverPending;
        m_view->viewport()->update();
    }
    if (!m_editor) {
        m_view->viewport()->setCursor(m_addMode ? Qt::CrossCursor
                                      : (hover >= 0 || hoverPending >= 0) ? Qt::IBeamCursor
                                                                           : Qt::ArrowCursor);
    }
    return true;
}

bool ReconstructionOverlay::handleMouseRelease(QMouseEvent *event)
{
    if (!m_active || !m_addDragging || event->button() != Qt::LeftButton)
        return false;

    m_addDragging = false;
    const int page = m_addPage;
    const double scale = m_view->m_scale;
    const QSizeF pageSize = page >= 0 && page < m_view->m_pageSizes.size()
                                ? m_view->m_pageSizes.at(page) : QSizeF();

    QRectF rect = QRectF(m_addStart, m_addEnd).normalized();
    if (scale > 0.0 && (rect.width() * scale < 12.0 || rect.height() * scale < 12.0))
        rect = QRectF(m_addStart, QSizeF(200.0, 40.0));

    if (pageSize.width() > 0.0 && pageSize.height() > 0.0) {
        rect.setWidth(qMin(rect.width(), pageSize.width()));
        rect.setHeight(qMin(rect.height(), pageSize.height()));
        rect.moveLeft(qBound(0.0, rect.left(), pageSize.width() - rect.width()));
        rect.moveTop(qBound(0.0, rect.top(), pageSize.height() - rect.height()));
    }

    setAddTextMode(false);
    startNewText(page, rect);
    return true;
}

void ReconstructionOverlay::setAddTextMode(bool on)
{
    if (on && !m_active)
        return;
    m_addDragging = false;
    if (m_addMode == on)
        return;

    m_addMode = on;
    m_view->viewport()->setCursor(on ? Qt::CrossCursor : Qt::ArrowCursor);
    m_view->viewport()->update();
    emit addTextModeChanged(on);
}

int ReconstructionOverlay::pendingIndexAt(int page, const QPointF &pagePoint) const
{
    for (int i = int(m_pending.size()) - 1; i >= 0; --i) {
        if (m_pending.at(i).edit.page == page && m_pending.at(i).box.contains(pagePoint)
            && !isDeletion(m_pending.at(i).edit))
            return i;
    }
    return -1;
}

int ReconstructionOverlay::paragraphAt(const QPointF &pagePoint) const
{
    if (!m_result)
        return -1;

    int best = -1;
    double bestArea = 0.0;
    for (int i = 0; i < int(m_result->paragraphs.size()); ++i) {
        const QRectF &box = m_result->paragraphs.at(i).bounds;
        if (!box.contains(pagePoint) || isStaged(m_page, box))
            continue;

        const double area = box.width() * box.height();
        if (best < 0 || area < bestArea) {
            best = i;
            bestArea = area;
        }
    }
    return best;
}

bool ReconstructionOverlay::isStaged(int page, const QRectF &originalBounds) const
{
    for (const PendingReconstructionEdit &pending : m_pending) {
        if (pending.edit.page == page && sameBounds(pending.edit.paragraph.bounds, originalBounds))
            return true;
    }
    return false;
}

// editor box

void ReconstructionOverlay::startEdit(int index)
{
    if (m_editor || !m_result || index < 0 || index >= int(m_result->paragraphs.size()))
        return;

    m_edited = m_result->paragraphs.at(index);
    m_editPage = m_page;
    m_editSource = m_source;
    m_region = Reconstruction::editRegion(*m_result, m_edited);
    m_baseline = Reconstruction::paragraphFirstBaselineY(*m_result, m_edited);
    m_offset = QPointF();
    m_newText = false;
    m_boxHeight = m_edited.bounds.height();
    openEditor();
}

void ReconstructionOverlay::startEditPending(int pendingIndex)
{
    if (m_editor || pendingIndex < 0 || pendingIndex >= int(m_pending.size()))
        return;

    const PendingReconstructionEdit &pending = m_pending.at(pendingIndex);
    m_edited = pending.edit.paragraph;
    m_editPage = pending.edit.page;
    m_editSource = int(pending.edit.fromTextLayer ? Reconstruction::PageSource::TextLayer
                                                  : Reconstruction::PageSource::Ocr);
    m_offset = pending.offset;
    m_region = pending.edit.region.translated(-pending.offset);
    m_baseline = pending.edit.firstBaselineY - pending.offset.y();
    m_newText = pending.edit.isNew;
    m_boxHeight = pending.boxHeight > 0.0 ? pending.boxHeight : m_edited.bounds.height();
    openEditor();
}

void ReconstructionOverlay::startNewText(int page, const QRectF &rect)
{
    if (m_editor || rect.isEmpty())
        return;

    Reconstruction::TextParagraph paragraph;
    paragraph.bounds = rect;
    paragraph.fontName = m_newTextStyle.fontName;
    paragraph.fontSize = m_newTextStyle.fontSize;
    paragraph.lineSpacing = double(m_newTextStyle.fontSize) * 1.2;
    paragraph.family = m_newTextStyle.family;
    paragraph.bold = m_newTextStyle.bold;
    paragraph.italic = m_newTextStyle.italic;
    paragraph.alignment = m_newTextAlignment;
    paragraph.textColor = m_newTextStyle.color;
    paragraph.backgroundColor = QColor(Qt::white);

    m_edited = paragraph;
    m_editPage = page;
    m_editSource = int(Reconstruction::PageSource::TextLayer);
    m_region = rect;
    m_baseline = rect.top() + m_newTextStyle.fontSize;
    m_offset = QPointF();
    m_newText = true;
    m_boxHeight = rect.height();
    openEditor();
}

void ReconstructionOverlay::openEditor()
{
    m_editScale = 0.0;
    m_hover = -1;
    m_hoverPending = -1;
    m_startOffset = m_offset;
    m_startRegion = m_region;
    m_startBoxHeight = m_boxHeight;
    m_moving = false;
    m_resizeCorner = -1;

    m_editor = new QTextEdit(m_view->viewport());
    m_editor->setFrameStyle(QFrame::NoFrame);
    m_editor->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_editor->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_editor->setWordWrapMode(QTextOption::WordWrap);
    m_editor->setAcceptRichText(false);
    m_editor->setStyleSheet(QStringLiteral(
        "QTextEdit { background: %1; border: 1px dashed #e08a00; padding: 0px; }")
        .arg(m_newText ? QStringLiteral("rgba(255,255,255,215)")
                       : m_edited.backgroundColor.name()));
    m_editor->installEventFilter(this);
    connect(m_editor, &QTextEdit::textChanged, this, [this]() { fitEditorHeight(); });
    connect(m_editor, &QTextEdit::currentCharFormatChanged, this, [this]() { emitFormat(); });

    m_handle = new MoveHandle(m_view->viewport());
    m_handle->installEventFilter(this);

    m_deleteHandle = new DeleteHandle(m_view->viewport());
    m_deleteHandle->installEventFilter(this);

    for (int corner = 0; corner < 4; ++corner) {
        m_resizeHandles[corner] = new ResizeHandle(m_view->viewport(), corner);
        m_resizeHandles[corner]->installEventFilter(this);
    }

    m_editor->show();
    layoutEditor();
    m_handle->show();
    m_deleteHandle->show();
    for (QWidget *handle : m_resizeHandles)
        handle->show();
    m_editor->setFocus();
    m_editor->moveCursor(QTextCursor::End);
    emit editingChanged(true);
    emitFormat();
    m_view->viewport()->update();
}

void ReconstructionOverlay::rebuildEditorDocument(double scale)
{
    QTextDocument *old = m_editor->document();
    const bool hasText = m_editScale > 0.0 && old;

    const bool weOwnOld = old && old->parent() == m_editor;

    int cursorPosition = 0;
    bool modified = false;
    if (hasText) {
        m_edited.runs = Reconstruction::runsFromDocument(old, m_editScale);
        m_edited.text = Reconstruction::joinRuns(m_edited.runs);
        if (m_newText)
            fitNewTextMetrics(m_edited, m_newTextStyle.fontSize);
        cursorPosition = m_editor->textCursor().position();
        modified = old->isModified();
    }

    std::unique_ptr<QTextDocument> document = Reconstruction::makeDocument(
        m_edited, scale, m_region.width() * scale);
    document->setParent(m_editor);
    QTextDocument *created = document.release();
    m_editor->setDocument(created);
    if (m_edited.markerGap > 0.0)
        m_editor->setTabStopDistance(m_edited.markerGap * scale);

    if (weOwnOld && old != created)
        old->deleteLater();

    QTextCursor cursor(created);
    cursor.setPosition(qMin(cursorPosition, qMax(0, created->characterCount() - 1)));
    m_editor->setTextCursor(cursor);
    created->setModified(modified);

    applyNewTextStyle(scale);

    m_editScale = scale;
}

void ReconstructionOverlay::layoutEditor()
{
    if (!m_editor || m_editPage < 0)
        return;

    const double scale = m_view->m_scale;
    if (!qFuzzyCompare(scale, m_editScale))
        rebuildEditorDocument(scale);

    if (m_newText)
        m_baseline = m_region.top()
                     + Reconstruction::firstBaselineOffset(m_editor->document()) / scale;

    const QRect pageRect = m_view->pageRectInViewport(m_editPage);
    const QRectF region(pageRect.x() + (m_region.x() + m_offset.x()) * scale,
                        pageRect.y() + (m_region.y() + m_offset.y()) * scale,
                        m_region.width() * scale, m_region.height() * scale);
    const QRectF box(pageRect.x() + (m_edited.bounds.x() + m_offset.x()) * scale,
                     pageRect.y() + (m_edited.bounds.y() + m_offset.y()) * scale,
                     m_edited.bounds.width() * scale, m_edited.bounds.height() * scale);

    m_minHeight = qMax(int(m_boxHeight * scale), 20) + 2;
    m_editor->setGeometry(int(region.x()) - 1, int(box.y()) - 1,
                          qRound(region.width()) + 2, m_minHeight);

    const double baselinePx = pageRect.y() + (m_baseline + m_offset.y()) * scale;
    m_editor->move(m_editor->x(),
                   qRound(baselinePx - Reconstruction::firstBaselineOffset(m_editor->document()))
                       - m_editor->viewport()->y());

    fitEditorHeight();
    positionHandle();
}

void ReconstructionOverlay::moveEditorTo(const QPointF &offset)
{
    if (!m_editor)
        return;

    QPointF clamped = offset;
    const QSizeF pageSize = m_editPage >= 0 && m_editPage < m_view->m_pageSizes.size()
                                ? m_view->m_pageSizes.at(m_editPage) : QSizeF();
    if (pageSize.width() > 0.0 && pageSize.height() > 0.0) {
        const QRectF bounds = m_newText
            ? QRectF(m_region.left(), m_edited.bounds.top(), m_region.width(), m_boxHeight)
            : m_edited.bounds;
        const double minX = -bounds.left();
        const double minY = -bounds.top();
        clamped.setX(qBound(minX, offset.x(), qMax(minX, pageSize.width() - bounds.right())));
        clamped.setY(qBound(minY, offset.y(), qMax(minY, pageSize.height() - bounds.bottom())));
    }

    if (qAbs(clamped.x()) < 0.01)
        clamped.setX(0.0);
    if (qAbs(clamped.y()) < 0.01)
        clamped.setY(0.0);

    m_offset = clamped;
    layoutEditor();
    m_view->viewport()->update();
}

void ReconstructionOverlay::positionHandle()
{
    if (!m_editor)
        return;

    const QRect box = m_editor->geometry();

    const QPoint corners[4] = { box.topLeft(), box.topRight(), box.bottomLeft(),
                                box.bottomRight() };
    for (int corner = 0; corner < 4; ++corner) {
        if (!m_resizeHandles[corner])
            continue;
        m_resizeHandles[corner]->move(corners[corner] - QPoint(5, 5));
        m_resizeHandles[corner]->raise();
    }

    if (m_handle) {
        m_handle->move(box.x() + (box.width() - m_handle->width()) / 2,
                       qMax(0, box.y() - m_handle->height()));
        m_handle->raise();
    }

    if (m_deleteHandle) {
        const int x = m_handle ? m_handle->x() + m_handle->width() + 3
                               : box.x() + (box.width() - m_deleteHandle->width()) / 2;
        m_deleteHandle->move(x, qMax(0, box.y() - m_deleteHandle->height()));
        m_deleteHandle->raise();
    }
}

void ReconstructionOverlay::resizeEditorTo(int corner, const QPointF &delta)
{
    if (!m_editor || m_editScale <= 0.0)
        return;

    constexpr double kMinWidth = 40.0;
    const bool leftSide = (corner == 0 || corner == 2);
    const bool topSide = (corner == 0 || corner == 1);
    const QSizeF pageSize = m_editPage >= 0 && m_editPage < m_view->m_pageSizes.size()
                                ? m_view->m_pageSizes.at(m_editPage) : QSizeF();

    double left = m_resizeStartRegion.left();
    double width = m_resizeStartRegion.width();
    if (leftSide) {
        double newLeft = qMin(left + delta.x(), left + width - kMinWidth);
        if (pageSize.width() > 0.0)
            newLeft = qMax(newLeft, -m_resizeStartOffset.x());
        width += left - newLeft;
        left = newLeft;
    } else {
        double newRight = qMax(left + width + delta.x(), left + kMinWidth);
        if (pageSize.width() > 0.0)
            newRight = qMin(newRight, pageSize.width() - m_resizeStartOffset.x());
        width = newRight - left;
    }

    const double minHeight = qMax(10.0, m_editor->document()->size().height() / m_editScale);
    double height = m_resizeStartHeight;
    QPointF offset = m_resizeStartOffset;
    if (topSide) {
        const double newHeight = qMax(minHeight, height - delta.y());
        double newOffsetY = offset.y() + (height - newHeight);
        newOffsetY = qMax(newOffsetY, -m_edited.bounds.top());
        height = qMax(minHeight, height - (newOffsetY - offset.y()));
        offset.setY(newOffsetY);
    } else {
        height = qMax(minHeight, height + delta.y());
        if (pageSize.height() > 0.0)
            height = qMax(minHeight, qMin(height, pageSize.height()
                                                      - (m_edited.bounds.top() + offset.y())));
    }
    if (qAbs(offset.y()) < 0.01)
        offset.setY(0.0);

    m_region = QRectF(left, m_region.top(), width, m_region.height());
    m_boxHeight = height;
    m_offset = offset;
    layoutEditor();
    m_view->viewport()->update();
}

void ReconstructionOverlay::fitEditorHeight()
{
    if (!m_editor)
        return;
    const int wanted = int(m_editor->document()->size().height()) + 2;
    m_editor->setFixedHeight(qMax(m_minHeight, wanted));
    positionHandle();
}

void ReconstructionOverlay::reposition()
{
    if (m_editor)
        layoutEditor();
}

bool ReconstructionOverlay::hasPendingEdit() const
{
    return !m_pending.isEmpty() || editorChanged();
}

bool ReconstructionOverlay::editorChanged() const
{
    return m_editor
           && (m_editor->document()->isModified() || m_offset != m_startOffset
               || m_region != m_startRegion
               || !qFuzzyCompare(m_boxHeight + 1.0, m_startBoxHeight + 1.0));
}

void ReconstructionOverlay::closeEditor()
{
    if (!m_editor)
        return;

    QTextEdit *editor = m_editor;
    m_editor = nullptr;
    m_offset = QPointF();
    m_moving = false;
    m_resizeCorner = -1;
    m_newText = false;
    for (QWidget *&handle : m_resizeHandles) {
        if (handle) {
            handle->hide();
            handle->deleteLater();
            handle = nullptr;
        }
    }
    if (m_handle) {
        m_handle->hide();
        m_handle->deleteLater();
        m_handle = nullptr;
    }
    if (m_deleteHandle) {
        m_deleteHandle->hide();
        m_deleteHandle->deleteLater();
        m_deleteHandle = nullptr;
    }
    editor->hide();
    editor->deleteLater();
    m_view->setFocus();
    m_view->viewport()->update();
    emit editingChanged(false);
}

void ReconstructionOverlay::cancelEdit()
{
    closeEditor();
}

void ReconstructionOverlay::commitEdit()
{
    if (!m_editor)
        return;

    const bool changed = editorChanged();

    PendingReconstructionEdit pending;
    ReconstructionEdit &edit = pending.edit;
    edit.page = m_editPage;
    edit.paragraph = m_edited;
    edit.paragraph.runs = Reconstruction::runsFromDocument(m_editor->document(), m_editScale);
    edit.paragraph.text = Reconstruction::joinRuns(edit.paragraph.runs);
    if (m_newText) {
        fitNewTextMetrics(edit.paragraph, m_newTextStyle.fontSize);
        if (m_editScale > 0.0)
            m_baseline = m_region.top()
                         + Reconstruction::firstBaselineOffset(m_editor->document()) / m_editScale;
    }
    edit.region = m_region.translated(m_offset);
    edit.firstBaselineY = m_baseline + m_offset.y();
    edit.fromTextLayer = (m_editSource == int(Reconstruction::PageSource::TextLayer));
    edit.isNew = m_newText;
    pending.offset = m_offset;
    pending.boxHeight = m_boxHeight;
    pending.box = editorBoxInPagePoints();

    closeEditor();

    if (pending.edit.isNew && pending.edit.paragraph.text.trimmed().isEmpty()) {
        removePending(pending.edit.page, pending.edit.paragraph.bounds);
        return;
    }

    if (changed)
        stage(std::move(pending));
}

QRectF ReconstructionOverlay::editorBoxInPagePoints() const
{
    const QRect pageRect = m_view->pageRectInViewport(m_editPage);
    const double scale = m_view->m_scale;
    const QRectF geometry = m_editor->geometry();
    return QRectF((geometry.x() - pageRect.x()) / scale, (geometry.y() - pageRect.y()) / scale,
                  geometry.width() / scale, geometry.height() / scale);
}

void ReconstructionOverlay::stage(PendingReconstructionEdit &&edit)
{
    for (int i = 0; i < int(m_pending.size()); ++i) {
        if (m_pending.at(i).edit.page == edit.edit.page
            && sameBounds(m_pending.at(i).edit.paragraph.bounds, edit.edit.paragraph.bounds)) {
            m_pending.removeAt(i);
            break;
        }
    }
    m_pending.append(std::move(edit));
    m_view->viewport()->update();
    emit pendingChanged(int(m_pending.size()));
}

QVector<PendingReconstructionEdit> ReconstructionOverlay::takePending()
{
    commitEdit();

    QVector<PendingReconstructionEdit> taken;
    taken.swap(m_pending);
    if (!taken.isEmpty()) {
        m_view->viewport()->update();
        emit pendingChanged(0);
    }
    return taken;
}

void ReconstructionOverlay::restorePending(const QVector<PendingReconstructionEdit> &edits)
{
    m_pending = edits;
    m_view->viewport()->update();
    emit pendingChanged(int(m_pending.size()));
}

bool ReconstructionOverlay::undoLastPending()
{
    if (m_pending.isEmpty())
        return false;

    m_pending.removeLast();
    m_view->viewport()->update();
    emit pendingChanged(int(m_pending.size()));
    return true;
}

void ReconstructionOverlay::discardPending()
{
    cancelEdit();
    if (m_pending.isEmpty())
        return;

    m_pending.clear();
    m_view->viewport()->update();
    emit pendingChanged(0);
}

void ReconstructionOverlay::flush()
{
    commitEdit();
    if (!m_pending.isEmpty())
        emit applyRequested();
}

// formatting

void ReconstructionOverlay::toggleFormat(int which)
{
    if (!m_editor)
        return;

    const ReconstructionFormat now = currentFormat();
    if (which == 0)
        setBold(!now.bold);
    else if (which == 1)
        setItalic(!now.italic);
    else
        setUnderlined(!now.underlined);
}

void ReconstructionOverlay::applyFormat(const QTextCharFormat &change)
{
    if (!m_editor)
        return;

    const bool wholeBox = m_newText && !m_editor->textCursor().hasSelection();
    if (wholeBox) {
        QTextDocument *document = m_editor->document();
        const bool wasEmpty = document->isEmpty();
        const bool modified = document->isModified();

        QTextCursor first(document);
        first.mergeBlockCharFormat(change);

        if (!wasEmpty) {
            QTextCursor all(document);
            all.select(QTextCursor::Document);
            all.mergeCharFormat(change);
        } else {
            document->setModified(modified);
        }
    }

    m_editor->mergeCurrentCharFormat(change);

    if (wholeBox)
        rememberNewTextStyle();
}

void ReconstructionOverlay::applyNewTextStyle(double scale)
{
    if (!m_editor || !m_newText || scale <= 0.0)
        return;

    QTextDocument *document = m_editor->document();
    if (!document->isEmpty())
        return;

    const QTextCharFormat format = Reconstruction::charFormat(m_newTextStyle, scale);
    const bool modified = document->isModified();

    QTextCursor first(document);
    first.setBlockCharFormat(format);
    document->setModified(modified);

    m_editor->setCurrentCharFormat(format);
}

void ReconstructionOverlay::rememberNewTextStyle()
{
    if (!m_editor || !m_newText || m_editScale <= 0.0)
        return;

    m_newTextStyle = styleFromFormat(m_editor->currentCharFormat(), m_editScale, m_newTextStyle);
}

void ReconstructionOverlay::removePending(int page, const QRectF &originalBounds)
{
    for (int i = 0; i < int(m_pending.size()); ++i) {
        if (m_pending.at(i).edit.page == page
            && sameBounds(m_pending.at(i).edit.paragraph.bounds, originalBounds)) {
            m_pending.removeAt(i);
            m_hoverPending = -1;
            m_view->viewport()->update();
            emit pendingChanged(int(m_pending.size()));
            return;
        }
    }
    m_view->viewport()->update();
}

void ReconstructionOverlay::deleteEdit()
{
    if (!m_editor)
        return;

    const int page = m_editPage;
    const QRectF bounds = m_edited.bounds;

    if (m_newText) {
        closeEditor();
        removePending(page, bounds);
        return;
    }

    PendingReconstructionEdit pending;
    ReconstructionEdit &edit = pending.edit;
    edit.page = page;
    edit.paragraph = m_edited;
    edit.paragraph.runs.clear();
    edit.paragraph.text.clear();
    edit.region = m_region;
    edit.firstBaselineY = m_baseline;
    edit.fromTextLayer = (m_editSource == int(Reconstruction::PageSource::TextLayer));
    edit.isNew = false;
    pending.offset = QPointF();
    pending.box = bounds;
    pending.boxHeight = bounds.height();

    closeEditor();
    stage(std::move(pending));
}

void ReconstructionOverlay::setBold(bool on)
{
    QTextCharFormat change;
    change.setFontWeight(on ? QFont::Bold : QFont::Normal);
    applyFormat(change);
}

void ReconstructionOverlay::setItalic(bool on)
{
    QTextCharFormat change;
    change.setFontItalic(on);
    applyFormat(change);
}

void ReconstructionOverlay::setUnderlined(bool on)
{
    QTextCharFormat change;
    change.setFontUnderline(on);
    applyFormat(change);
}

void ReconstructionOverlay::setAlignment(Qt::AlignmentFlag alignment)
{
    if (!m_editor || m_edited.alignment == alignment)
        return;

    m_edited.alignment = alignment;
    if (m_newText)
        m_newTextAlignment = alignment;

    QTextDocument *document = m_editor->document();
    const bool wasEmpty = document->isEmpty();
    const bool modified = document->isModified();

    if (alignment == Qt::AlignJustify && !m_newText)
        reflowForJustify();

    QTextOption option = document->defaultTextOption();
    option.setAlignment(alignment);
    document->setDefaultTextOption(option);

    QTextBlockFormat format;
    format.setAlignment(alignment);
    QTextCursor all(document);
    all.select(QTextCursor::Document);
    all.mergeBlockFormat(format);

    if (wasEmpty && m_newText)
        document->setModified(modified);

    emitFormat();
}

void ReconstructionOverlay::reflowForJustify()
{
    if (!m_editor)
        return;

    QTextDocument *document = m_editor->document();
    document->documentLayout()->documentSize();

    double widest = 0.0;
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        const QTextLayout *layout = block.layout();
        for (int i = 0; layout && i < layout->lineCount(); ++i)
            widest = qMax(widest, layout->lineAt(i).naturalTextWidth());
    }
    if (widest <= 0.0)
        return;

    struct Join
    {
        int separator = 0;
        bool space = true;
    };
    QVector<Join> joins;

    for (QTextBlock block = document->begin(); block.isValid() && block.next().isValid();
         block = block.next()) {
        const QTextLayout *layout = block.layout();
        if (!layout || layout->lineCount() == 0)
            continue;

        const QString text = block.text();
        const QString nextText = block.next().text();
        if (text.trimmed().isEmpty() || nextText.trimmed().isEmpty())
            continue;
        if (layout->lineAt(layout->lineCount() - 1).naturalTextWidth() < 0.75 * widest)
            continue;

        const int tab = nextText.indexOf(QLatin1Char('\t'));
        if (tab > 0 && tab <= 4)
            continue;

        Join join;
        join.separator = block.position() + block.length() - 1;
        join.space = !text.endsWith(QLatin1Char('-')) && !text.endsWith(QLatin1Char(' '));
        joins.append(join);
    }
    if (joins.isEmpty())
        return;

    QTextCursor cursor(document);
    cursor.beginEditBlock();
    for (int i = int(joins.size()) - 1; i >= 0; --i) {
        cursor.setPosition(joins.at(i).separator);
        cursor.deleteChar();
        if (joins.at(i).space)
            cursor.insertText(QStringLiteral(" "));
    }
    cursor.endEditBlock();
}

void ReconstructionOverlay::setTextColor(const QColor &color)
{
    QTextCharFormat change;
    change.setForeground(color);
    applyFormat(change);
}

void ReconstructionOverlay::setFontName(const QString &family)
{
    if (!m_editor || family.isEmpty())
        return;

    QFont font;
    font.setFamily(family);

    QTextCharFormat change;
    change.setFont(font, QTextCharFormat::FontPropertiesSpecifiedOnly);

    change.setProperty(Reconstruction::kFontNameProperty, family);

    change.setProperty(Reconstruction::kFontFamilyProperty,
                       int(Reconstruction::classifyFont(family).family));
    applyFormat(change);
}

void ReconstructionOverlay::setFontSize(double points)
{
    if (!m_editor || points <= 0.0 || m_editScale <= 0.0)
        return;

    QFont font;
    font.setPixelSize(qMax(6, qRound(points * m_editScale)));

    QTextCharFormat change;
    change.setFont(font, QTextCharFormat::FontPropertiesSpecifiedOnly);
    change.setProperty(Reconstruction::kFontSizeProperty, points);
    applyFormat(change);

    refitLineHeight();
}

void ReconstructionOverlay::refitLineHeight()
{
    if (!m_editor || m_editScale <= 0.0)
        return;

    QTextDocument *document = m_editor->document();

    double biggest = 0.0;
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid())
                continue;

            const QTextCharFormat format = fragment.charFormat();
            double size = format.property(Reconstruction::kFontSizeProperty).toDouble();
            if (size <= 0.0)
                size = format.font().pixelSize() / m_editScale;
            biggest = qMax(biggest, size);
        }
    }

    biggest = qMax(biggest,
                   m_editor->currentCharFormat().property(Reconstruction::kFontSizeProperty)
                       .toDouble());

    const double spacing = (m_newText && biggest > 0.0)
                               ? biggest * 1.2
                               : Reconstruction::lineSpacingFor(m_edited, biggest);

    QTextBlockFormat blockFormat;
    blockFormat.setLineHeight(spacing * m_editScale, QTextBlockFormat::FixedHeight);

    QTextCursor all(document);
    all.select(QTextCursor::Document);
    all.mergeBlockFormat(blockFormat);

    layoutEditor();
}

ReconstructionFormat ReconstructionOverlay::currentFormat() const
{
    ReconstructionFormat result;
    if (!m_editor)
        return result;

    const QTextCharFormat format = m_editor->currentCharFormat();

    int kind = format.property(Reconstruction::kFontFamilyProperty).toInt();
    if (kind <= int(Reconstruction::FontFamily::Unknown)
        || kind > int(Reconstruction::FontFamily::Mono)) {
        kind = int(Reconstruction::classifyFont(
                       format.property(Reconstruction::kFontNameProperty).toString()).family);
    }
    result.family = kind == int(Reconstruction::FontFamily::Unknown)
                        ? Reconstruction::FontFamily::Sans
                        : Reconstruction::FontFamily(kind);
    result.fontFamily = format.font().family();

    double size = format.property(Reconstruction::kFontSizeProperty).toDouble();
    if (size <= 0.0 && m_editScale > 0.0)
        size = format.font().pixelSize() / m_editScale;
    if (size > 0.0)
        result.sizePoints = size;

    const QBrush brush = format.foreground();
    result.color = brush.style() != Qt::NoBrush ? brush.color() : m_edited.textColor;

    result.bold = format.font().bold();
    result.italic = format.font().italic();
    result.underlined = format.fontUnderline();
    result.alignment = m_edited.alignment;
    return result;
}

void ReconstructionOverlay::emitFormat()
{
    if (m_editor)
        emit formatChanged(currentFormat());
}

bool ReconstructionOverlay::eventFilter(QObject *watched, QEvent *event)
{
    if (m_handle && watched == m_handle) {
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton) {
                m_moving = true;
                m_moveStartMouse = mouse->globalPosition();
                m_moveStartOffset = m_offset;
            }
            if (m_editor)
                m_editor->setFocus(Qt::MouseFocusReason);
            return true;
        }
        case QEvent::MouseMove:
            if (m_moving && m_view->m_scale > 0.0) {
                const QPointF moved =
                    static_cast<QMouseEvent *>(event)->globalPosition() - m_moveStartMouse;
                moveEditorTo(m_moveStartOffset + moved / m_view->m_scale);
            }
            return true;
        case QEvent::MouseButtonRelease:
            m_moving = false;
            return true;
        default:
            break;
        }
    }

    if (m_deleteHandle && watched == m_deleteHandle) {
        switch (event->type()) {
        case QEvent::MouseButtonPress:
            if (m_editor)
                m_editor->setFocus(Qt::MouseFocusReason);
            return true;
        case QEvent::MouseButtonRelease: {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton
                && m_deleteHandle->rect().contains(mouse->position().toPoint()))
                deleteEdit();
            return true;
        }
        default:
            break;
        }
    }

    for (int corner = 0; corner < 4; ++corner) {
        if (!m_resizeHandles[corner] || watched != m_resizeHandles[corner])
            continue;

        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton) {
                m_resizeCorner = corner;
                m_resizeStartMouse = mouse->globalPosition();
                m_resizeStartRegion = m_region;
                m_resizeStartOffset = m_offset;
                m_resizeStartHeight = m_boxHeight;
            }
            if (m_editor)
                m_editor->setFocus(Qt::MouseFocusReason);
            return true;
        }
        case QEvent::MouseMove:
            if (m_resizeCorner == corner && m_view->m_scale > 0.0) {
                const QPointF moved =
                    static_cast<QMouseEvent *>(event)->globalPosition() - m_resizeStartMouse;
                resizeEditorTo(corner, moved / m_view->m_scale);
            }
            return true;
        case QEvent::MouseButtonRelease:
            m_resizeCorner = -1;
            return true;
        default:
            break;
        }
    }

    if (m_editor && watched == m_editor) {
        if (event->type() == QEvent::ShortcutOverride) {
            auto *key = static_cast<QKeyEvent *>(event);
            const Qt::KeyboardModifiers modifiers = key->modifiers();
            const bool onlyControl = (modifiers & Qt::ControlModifier)
                                     && !(modifiers & (Qt::ShiftModifier | Qt::AltModifier));
            if (onlyControl && (key->key() == Qt::Key_B || key->key() == Qt::Key_I
                                || key->key() == Qt::Key_U)) {
                event->accept();
                return true;
            }
        } else if (event->type() == QEvent::KeyPress) {
            auto *key = static_cast<QKeyEvent *>(event);

            if (key->key() == Qt::Key_Escape) {
                cancelEdit();
                return true;
            }

            const bool control = key->modifiers() & Qt::ControlModifier;
            const bool isEnter = key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter;
            if (isEnter && control) {
                commitEdit();
                return true;
            }
            if ((key->modifiers() & Qt::AltModifier) && !control) {
                const double step = (key->modifiers() & Qt::ShiftModifier) ? 10.0 : 1.0;
                switch (key->key()) {
                case Qt::Key_Left:  moveEditorTo(m_offset + QPointF(-step, 0.0)); return true;
                case Qt::Key_Right: moveEditorTo(m_offset + QPointF(step, 0.0)); return true;
                case Qt::Key_Up:    moveEditorTo(m_offset + QPointF(0.0, -step)); return true;
                case Qt::Key_Down:  moveEditorTo(m_offset + QPointF(0.0, step)); return true;
                case Qt::Key_Delete: deleteEdit(); return true;
                default: break;
                }
            }
            if (control && key->key() == Qt::Key_B) { toggleFormat(0); return true; }
            if (control && key->key() == Qt::Key_I) { toggleFormat(1); return true; }
            if (control && key->key() == Qt::Key_U) { toggleFormat(2); return true; }
        } else if (event->type() == QEvent::FocusOut) {
            const Qt::FocusReason reason = static_cast<QFocusEvent *>(event)->reason();
            if (reason != Qt::ActiveWindowFocusReason && reason != Qt::PopupFocusReason) {
                QTimer::singleShot(0, this, [this]() {
                    if (!m_editor || m_editor->hasFocus() || QApplication::activePopupWidget())
                        return;
                    if (isStyleControl(QApplication::focusWidget())
                        || isStyleControl(QApplication::widgetAt(QCursor::pos())))
                        return;
                    commitEdit();
                });
            }
        }
    }
    return QObject::eventFilter(watched, event);
}