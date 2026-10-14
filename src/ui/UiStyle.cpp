#include "UiStyle.h"

#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QVBoxLayout>

namespace ui {

namespace {

const char* roleName(ButtonRole role)
{
    switch (role) {
    case ButtonRole::Primary: return "primary";
    case ButtonRole::Success: return "success";
    case ButtonRole::Danger: return "danger";
    case ButtonRole::Secondary: break;
    }
    return "secondary";
}

const char* toneName(Tone tone)
{
    switch (tone) {
    case Tone::Info: return "info";
    case Tone::Ok: return "ok";
    case Tone::Warn: return "warn";
    case Tone::Error: return "error";
    case Tone::Neutral: break;
    }
    return "neutral";
}

void repolish(QWidget* widget)
{
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

}  // namespace

void setButtonRole(QPushButton* button, ButtonRole role)
{
    button->setCursor(Qt::PointingHandCursor);
    const QString name = QString::fromLatin1(roleName(role));
    if (button->property("role").toString() == name) {
        return;
    }
    button->setProperty("role", name);
    repolish(button);
}

void setTone(QWidget* widget, Tone tone)
{
    const QString name = QString::fromLatin1(toneName(tone));
    if (widget->property("tone").toString() == name) {
        return;
    }
    widget->setProperty("tone", name);
    repolish(widget);
}

QColor okColor()
{
    return QColor(0x2e, 0xa0, 0x43);
}

QColor warnColor()
{
    return QColor(0xd2, 0x99, 0x22);
}

QColor errorColor()
{
    return QColor(0xf8, 0x51, 0x49);
}

QLabel* captionLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setProperty("role", QStringLiteral("caption"));
    return label;
}

QLabel* hintLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setProperty("role", QStringLiteral("hint"));
    label->setWordWrap(true);
    return label;
}

void applyGroupLayout(QLayout* layout)
{
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(kInnerGap);
}

QScrollArea* scrollPage(QWidget* content, QWidget* parent)
{
    auto* area = new QScrollArea(parent);
    area->setObjectName(QStringLiteral("scrollPage"));
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    area->setWidget(content);
    return area;
}

}  // namespace ui

StatTile::StatTile(const QString& caption, QWidget* parent)
    : QFrame(parent)
    , m_caption(new QLabel(caption, this))
    , m_value(new QLabel(QStringLiteral("—"), this))
{
    setProperty("role", QStringLiteral("tile"));
    m_caption->setProperty("role", QStringLiteral("tileCaption"));
    m_value->setProperty("role", QStringLiteral("tileValue"));
    m_caption->setAlignment(Qt::AlignCenter);
    m_value->setAlignment(Qt::AlignCenter);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(1);
    layout->addWidget(m_caption);
    layout->addWidget(m_value);
}

void StatTile::setValue(const QString& value)
{
    m_value->setText(value);
}

void StatTile::setCaption(const QString& caption)
{
    m_caption->setText(caption);
}

QString StatTile::value() const
{
    return m_value->text();
}

void StatTile::setTone(ui::Tone tone)
{
    ui::setTone(this, tone);
}
