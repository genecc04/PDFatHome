#pragma once

#include <QPointF>
#include <QtGlobal>

// Hit-testing helpers shared by the eraser preview (PageView) and the real
// erase (PdfEditor), which must agree on what a stroke touches.
namespace Geometry {

inline double pointSegmentDistanceSquared(const QPointF &point, const QPointF &start,
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

inline double crossProduct(const QPointF &a, const QPointF &b, const QPointF &c)
{
    return (b.x() - a.x()) * (c.y() - a.y())
           - (b.y() - a.y()) * (c.x() - a.x());
}

inline bool pointOnSegment(const QPointF &point, const QPointF &start, const QPointF &end)
{
    constexpr double epsilon = 1e-9;
    return qAbs(crossProduct(start, end, point)) <= epsilon
           && point.x() >= qMin(start.x(), end.x()) - epsilon
           && point.x() <= qMax(start.x(), end.x()) + epsilon
           && point.y() >= qMin(start.y(), end.y()) - epsilon
           && point.y() <= qMax(start.y(), end.y()) + epsilon;
}

inline bool segmentsIntersect(const QPointF &a, const QPointF &b,
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

inline double segmentDistanceSquared(const QPointF &a, const QPointF &b,
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
