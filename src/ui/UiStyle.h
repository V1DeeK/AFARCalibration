#pragma once

#include <QColor>
#include <QFrame>
#include <QString>

class QLabel;
class QLayout;
class QPushButton;
class QScrollArea;
class QWidget;

/// Общие приёмы оформления: роли кнопок, тона подписей, плитки, прокручиваемые страницы.
/// Сами цвета и отступы лежат в таблице стилей (Theme.cpp) и меняются вместе с темой.
namespace ui {

/// Единые размеры: поля страниц, расстояние между окошками и внутри них.
inline constexpr int kPageMargin = 12;
inline constexpr int kGap = 10;
inline constexpr int kInnerGap = 8;

enum class ButtonRole {
    Secondary,  ///< обычная кнопка: белая с рамкой
    Primary,    ///< главное действие в окошке: синяя
    Success,    ///< запуск (зелёная)
    Danger,     ///< остановка, отмена (красная)
};

enum class Tone { Neutral, Info, Ok, Warn, Error };

void setButtonRole(QPushButton* button, ButtonRole role);
/// Тон подписи или плитки: цветная «таблетка» в таблице стилей.
void setTone(QWidget* widget, Tone tone);

/// Цвета состояний для ячеек таблиц: читаются и на светлом, и на тёмном фоне.
QColor okColor();
QColor warnColor();
QColor errorColor();

/// Подпись-заголовок мелким приглушённым шрифтом над полем.
QLabel* captionLabel(const QString& text, QWidget* parent);
/// Подпись-подсказка: приглушённая, с переносом строк.
QLabel* hintLabel(const QString& text, QWidget* parent);

/// Отступы и расстояния окошка по умолчанию.
void applyGroupLayout(QLayout* layout);

/// Страница вкладки: прокручиваемая, без рамки. Содержимое растягивается по окну,
/// но не становится меньше своего минимума, поэтому ничего не налезает друг на друга.
QScrollArea* scrollPage(QWidget* content, QWidget* parent = nullptr);

}  // namespace ui

/// Плитка «подпись + значение» для счётчиков и состояний.
class StatTile final : public QFrame {
    Q_OBJECT

public:
    explicit StatTile(const QString& caption, QWidget* parent = nullptr);

    void setValue(const QString& value);
    void setCaption(const QString& caption);
    void setTone(ui::Tone tone);
    [[nodiscard]] QString value() const;

private:
    QLabel* m_caption = nullptr;
    QLabel* m_value = nullptr;
};
