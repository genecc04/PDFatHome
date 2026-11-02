#pragma once
#include <QColor>
#include <QIcon>
#include <QToolButton>

class ColorMenuButton : public QToolButton
{
    Q_OBJECT
public:
    explicit ColorMenuButton(const QColor &initial, QWidget *parent = nullptr);

    QColor color() const { return m_color; }
    void setColor(const QColor &color);

signals:
    void colorChanged(const QColor &color);

private:
    void chooseCustomColor();
    QIcon swatch(const QColor &color) const;

    QColor m_color;
};