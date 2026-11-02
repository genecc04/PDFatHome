#include "ColorMenuButton.h"

#include <QColorDialog>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QVector>

ColorMenuButton::ColorMenuButton(const QColor &initial, QWidget *parent)
    : QToolButton(parent)
    , m_color(initial)
{
    setPopupMode(QToolButton::InstantPopup);
    setFocusPolicy(Qt::NoFocus);
    setIconSize(QSize(18, 18));
    setToolTip("Color");

    auto *menu = new QMenu(this);

    static const QVector<QPair<QString, QColor>> presets = {
        { "Black", QColor(0, 0, 0) },
        { "Dark gray", QColor(90, 90, 90) },
        { "Blue", QColor(0, 51, 204) },
        { "Red", QColor(204, 0, 0) },
        { "Green", QColor(0, 140, 60) },
        { "Orange", QColor(255, 140, 0) },
        { "Yellow", QColor(255, 220, 0) },
        { "Purple", QColor(130, 40, 170) },
        { "White", QColor(255, 255, 255) },
    };
    for (const auto &preset : presets) {
        const QColor color = preset.second;
        QAction *action = menu->addAction(swatch(color), preset.first);
        connect(action, &QAction::triggered, this, [this, color]() { setColor(color); });
    }

    menu->addSeparator();
    QAction *more = menu->addAction("More colors...");
    connect(more, &QAction::triggered, this, &ColorMenuButton::chooseCustomColor);

    setMenu(menu);
    setStyleSheet(
        "QToolButton::menu-indicator {"
        " subcontrol-position: bottom right;"
        " right: 5px; bottom: 2px;"
        " width: 0px; height: 0px;"
        "}"
    );
    setIcon(swatch(m_color));
}

void ColorMenuButton::setColor(const QColor &color)
{
    if (!color.isValid() || color == m_color)
        return;
    m_color = color;
    setIcon(swatch(m_color));
    emit colorChanged(m_color);
}

void ColorMenuButton::chooseCustomColor()
{
    const QColor chosen = QColorDialog::getColor(m_color, this, "Choose a color");
    if (chosen.isValid())
        setColor(chosen);
}

QIcon ColorMenuButton::swatch(const QColor &color) const
{
    constexpr int size = 20;
    const qreal dpr = devicePixelRatioF();
    QPixmap pixmap(QSize(size, size) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(color);
    QPainter painter(&pixmap);
    painter.setPen(QColor(90, 90, 90));
    painter.drawRect(0, 0, size - 1, size - 1);
    return QIcon(pixmap);
}