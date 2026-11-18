#pragma once

#include <QColor>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QString>
#include <QVector>

#include <memory>

#include "reconstruction/ReconstructedPage.h"

class PageView;
class QEvent;
class QMouseEvent;
class QPainter;
class QTextCharFormat;
class QTextDocument;
class QTextEdit;
class QWidget;

struct ReconstructionEdit
{
    int page = -1;
    Reconstruction::TextParagraph paragraph;
    QRectF region;
    double firstBaselineY = 0.0;
    bool fromTextLayer = false;
};

struct PendingReconstructionEdit
{
    ReconstructionEdit edit;
    QPointF offset;
    QRectF box;
    std::shared_ptr<QTextDocument> preview;
    double previewScale = 0.0;
};

struct ReconstructionFormat
{
    Reconstruction::FontFamily family = Reconstruction::FontFamily::Sans;
    QString fontFamily;
    double sizePoints = 12.0;
    QColor color = QColor(Qt::black);
    bool bold = false;
    bool italic = false;
    bool underlined = false;
};

class ReconstructionOverlay : public QObject
{
    Q_OBJECT
public:
    explicit ReconstructionOverlay(PageView *view);

    bool isActive() const { return m_active; }

    bool isEditing() const { return m_editor != nullptr; }

    void setFontName(const QString &installedFamily);
    void setFontSize(double points);
    void setTextColor(const QColor &color);
    void setBold(bool on);
    void setItalic(bool on);
    void setUnderlined(bool on);
    void setActive(bool active);

    void setResult(int page, std::shared_ptr<const Reconstruction::ReconstructedPage> result,
                   int source);
    void reset();

    // Hooks for PageView.
    void paint(QPainter &painter, int page, const QRect &target);
    bool handleMousePress(QMouseEvent *event);
    bool handleMouseMove(const QPoint &pos);
    void reposition();
    bool hasPendingEdit() const;
    void commitEdit();
    void cancelEdit();

    int pendingCount() const { return int(m_pending.size()); }
    QVector<PendingReconstructionEdit> takePending();
    void restorePending(const QVector<PendingReconstructionEdit> &edits);
    bool undoLastPending();
    void discardPending();
    void flush();

signals:
    void pendingChanged(int count);
    void applyRequested();
    void editingChanged(bool editing);
    void formatChanged(const ReconstructionFormat &format);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void startEdit(int index);
    void startEditPending(int pendingIndex);
    void openEditor();
    void stage(PendingReconstructionEdit &&edit);
    int pendingIndexAt(int page, const QPointF &pagePoint) const;
    int paragraphAt(const QPointF &pagePoint) const;
    bool isStaged(int page, const QRectF &originalBounds) const;
    QTextDocument *previewDocument(PendingReconstructionEdit &pending, double scale);
    void drawPending(QPainter &painter, int page, const QRect &target);
    QRectF editorBoxInPagePoints() const;
    void closeEditor();
    void layoutEditor();
    void moveEditorTo(const QPointF &offset);
    void positionHandle();
    void rebuildEditorDocument(double scale);
    void fitEditorHeight();
    void toggleFormat(int which);
    void applyFormat(const QTextCharFormat &change);
    void refitLineHeight();
    void emitFormat();
    ReconstructionFormat currentFormat() const;

    PageView *m_view = nullptr;
    bool m_active = false;

    std::shared_ptr<const Reconstruction::ReconstructedPage> m_result;
    int m_page = -1;
    int m_source = 0;
    int m_hover = -1;
    int m_hoverPending = -1;

    QVector<PendingReconstructionEdit> m_pending;

    QTextEdit *m_editor = nullptr;
    int m_editPage = -1;
    int m_editSource = 0;
    Reconstruction::TextParagraph m_edited;
    QRectF m_region;
    double m_baseline = 0.0;
    double m_editScale = 0.0;
    int m_minHeight = 20;

    QWidget *m_handle = nullptr;
    QPointF m_offset;
    QPointF m_startOffset;
    bool m_moving = false;
    QPointF m_moveStartMouse;
    QPointF m_moveStartOffset;
};