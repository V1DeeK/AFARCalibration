#include "Theme.h"

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QPalette>
#include <QSettings>
#include <QStandardPaths>
#include <QStyleFactory>

#include <iterator>
#include <utility>
#include <vector>

namespace {

AppTheme g_theme = AppTheme::Light;

/// Цвета одной темы. Всё оформление берётся отсюда, чтобы светлая и тёмная
/// темы отличались только значениями, а не правилами.
struct Tokens {
    const char* window;
    const char* card;
    const char* alt;           ///< чередующиеся строки, шапки таблиц, плитки
    const char* border;
    const char* borderStrong;
    const char* text;
    const char* muted;
    const char* disabled;
    const char* accent;
    const char* accentHover;
    const char* accentPressed;
    const char* accentSoft;    ///< выделение, «информационные» плашки
    const char* accentText;    ///< текст акцентного цвета на мягком фоне
    const char* success;
    const char* successHover;
    const char* danger;
    const char* dangerHover;
    const char* warn;
    const char* okSoft;
    const char* okText;
    const char* warnSoft;
    const char* warnText;
    const char* errorSoft;
    const char* errorText;
    const char* hoverFill;     ///< лёгкая подсветка под курсором
    const char* handle;        ///< ползунок прокрутки
    const char* handleHover;
};

// Палитра «графит и кобальт». Один нейтральный ряд с лёгким холодным оттенком (тон ~218°),
// один акцент и три смысловых цвета, сведённые по насыщенности и светлоте, чтобы ни один
// не кричал громче другого. Чистых #000/#fff в тексте и фоне тёмной темы нет: глубина в
// тёмной теме — светлотой слоя, а не тенью. Контраст текста на фоне — не ниже 4,5:1.
constexpr Tokens kLight{
    "#f4f5f7", "#ffffff", "#f0f1f4", "#e2e4e9", "#cdd1d8", "#1b1f27", "#5e6573", "#a3a9b4",
    "#2f5fbf", "#2852a8", "#224690", "rgba(47,95,191,0.10)", "#244c9c",
    "#2e7d4f", "#276b44", "#c2412d", "#a93827", "#b7791f",
    "rgba(46,125,79,0.12)", "#22633d", "rgba(183,121,31,0.14)", "#7a4f12",
    "rgba(194,65,45,0.11)", "#9c3324",
    "rgba(27,31,39,0.05)", "rgba(27,31,39,0.22)", "rgba(27,31,39,0.38)"};

constexpr Tokens kDark{
    "#15171c", "#1d2027", "#242831", "#2e333d", "#3c424e", "#e7e9ee", "#9aa1ae", "#5d6470",
    "#4a74d0", "#5a82da", "#3f66ba", "rgba(108,147,230,0.18)", "#a9c1f2",
    "#3e9a63", "#47a86d", "#c95a45", "#d66a55", "#d19a3e",
    "rgba(62,154,99,0.20)", "#8ed3a8", "rgba(209,154,62,0.20)", "#e9c27e",
    "rgba(210,86,63,0.20)", "#f0a08f",
    "rgba(231,233,238,0.06)", "rgba(231,233,238,0.20)", "rgba(231,233,238,0.34)"};

/// Ряды графиков: та же светлота и насыщенность, разный тон; порядок различим
/// и при нарушениях цветового зрения (синий, охра, бирюза, слива, кирпич, графит).
constexpr const char* kSeriesLight[6] = {"#2f5fbf", "#c07a1e", "#1f8a7a", "#8a4fa8", "#c2412d", "#5e6573"};
constexpr const char* kSeriesDark[6] = {"#6c93e6", "#e0a04a", "#4db8a6", "#b585d0", "#e07a63", "#9aa1ae"};

const Tokens& tokens()
{
    return g_theme == AppTheme::Dark ? kDark : kLight;
}

/// Значки для индикаторов (галочка и точка): таблица стилей Qt берёт их из файлов.
/// Рисуем их при запуске в каталог кэша, чтобы не тащить ресурсы; @2x — для Retina.
/// Пустая строка — не получилось записать, тогда индикатор остаётся просто залитым.
QString iconUrl(const QString& name)
{
    static const QString dir = [] {
        const QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        const QString path = base + QStringLiteral("/ui-icons-v1");
        return QDir().mkpath(path) ? path : QString();
    }();
    if (dir.isEmpty()) {
        return {};
    }
    const auto draw = [&name](int size, const QString& file) {
        if (QFile::exists(file)) {
            return true;
        }
        QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing);
        const double k = size / 16.0;
        if (name == QLatin1String("check")) {
            QPen pen(Qt::white, 2.0 * k, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            p.setPen(pen);
            p.drawPolyline(QPolygonF{QPointF(3.6 * k, 8.4 * k), QPointF(6.6 * k, 11.4 * k), QPointF(12.4 * k, 4.8 * k)});
        } else {
            p.setPen(Qt::NoPen);
            p.setBrush(Qt::white);
            p.drawEllipse(QPointF(8 * k, 8 * k), 3.2 * k, 3.2 * k);
        }
        p.end();
        return image.save(file);
    };
    const QString file1 = dir + QLatin1Char('/') + name + QStringLiteral(".png");
    const QString file2 = dir + QLatin1Char('/') + name + QStringLiteral("@2x.png");
    if (!draw(16, file1) || !draw(32, file2)) {
        return {};
    }
    return QStringLiteral("url(\"%1\")").arg(QDir::fromNativeSeparators(file1));
}

QString build(const Tokens& t)
{
    QString css = QStringLiteral(R"CSS(
/* ---------- основа ---------- */
QWidget { color: @text@; font-size: 13px; }
QLabel { background: transparent; }
QWidget:disabled { color: @disabled@; }
QMainWindow, QDialog, QWizard, QWizardPage { background: @window@; }
QToolTip { background: @card@; color: @text@; border: 1px solid @borderStrong@; border-radius: 6px; padding: 5px 8px; }

/* ---------- окошки-карточки ---------- */
QGroupBox { background: @card@; border: 1px solid @border@; border-radius: 10px;
            margin-top: 0px; padding: 40px 16px 16px 16px; }
QGroupBox::title { subcontrol-origin: padding; subcontrol-position: top left; left: 16px; top: 14px;
                   padding: 0px; color: @text@; font-weight: 600; }
QGroupBox::indicator { width: 16px; height: 16px; border: 1px solid @borderStrong@; border-radius: 4px;
                       background: @card@; }
QGroupBox::indicator:checked { background: @accent@; border-color: @accent@; image: @check@; }
QFrame#ConnectionBar { background: @card@; border: 1px solid @border@; border-radius: 10px; }

/* ---------- вкладки ---------- */
QTabWidget::pane { border: 1px solid @border@; border-radius: 10px; top: -1px; background: @card@; }
QTabBar::tab { background: transparent; color: @muted@; padding: 7px 14px; margin-right: 2px;
               border: none; border-bottom: 2px solid transparent; }
QTabBar::tab:hover:!selected { color: @text@; }
QTabBar::tab:selected { color: @text@; font-weight: 600; border-bottom: 2px solid @accent@; }
QTabBar::tab:disabled { color: @disabled@; }
/* Главная навигация — сегментированный переключатель: выбранный раздел приподнят, а не залит цветом. */
QTabBar#mainTabBar { background: transparent; border: none; }
QTabBar#mainTabBar::tab { padding: 6px 16px; margin: 6px 2px 6px 0px; border: 1px solid transparent;
                          border-radius: 7px; color: @muted@; }
QTabBar#mainTabBar::tab:selected { background: @card@; color: @text@; font-weight: 600; border-color: @borderStrong@; }
QTabBar#mainTabBar::tab:hover:!selected { background: @hoverFill@; color: @text@; }
/* Стрелки прокрутки у вкладок «Модулей» в узком окне — во всю свою ширину. */
QTabBar::scroller { width: 64px; }
QTabWidget#mainTabs::pane { border: none; background: transparent; border-radius: 0px; top: 0px; }
QTabWidget#mainTabs::tab-bar { left: 12px; }
QTabWidget#calibrationTabs QTabBar::tab, QTabWidget#vnaTabs QTabBar::tab { padding: 6px 7px; margin-right: 0px; font-size: 12px; }
QTabWidget#heterFreqTabs::pane { top: 0px; border: none; background: transparent; border-radius: 0px; }
QTabWidget#heterFreqTabs QTabBar::tab { padding: 6px 8px; margin-right: 0px; font-size: 12px; }
QTabWidget#heterFreqTabs QTabBar::scroller { width: 16px; }

/* ---------- поля ввода ---------- */
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QPlainTextEdit, QTextEdit {
    background: @card@; border: 1px solid @borderStrong@; border-radius: 7px; padding: 4px 9px; min-height: 20px;
    selection-background-color: @accent@; selection-color: #ffffff; }
QPlainTextEdit, QTextEdit { padding: 6px 8px; }
QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover, QComboBox:hover { border-color: @muted@; }
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus, QPlainTextEdit:focus, QTextEdit:focus {
    border: 1px solid @accent@; }
QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled, QComboBox:disabled {
    background: @alt@; color: @disabled@; border-color: @border@; }
QPlainTextEdit[readOnly="true"], QTextEdit[readOnly="true"] { background: @alt@; }
QComboBox QAbstractItemView { background: @card@; border: 1px solid @border@; border-radius: 8px;
    selection-background-color: @accentSoft@; selection-color: @text@; outline: 0px; padding: 4px; }
QCheckBox, QRadioButton { spacing: 8px; }
QCheckBox::indicator, QAbstractItemView::indicator { width: 16px; height: 16px; border: 1px solid @borderStrong@;
    border-radius: 5px; background: @card@; }
QCheckBox::indicator:hover, QAbstractItemView::indicator:hover { border-color: @accent@; }
QCheckBox::indicator:checked, QAbstractItemView::indicator:checked { background: @accent@; border-color: @accent@;
    image: @check@; }
QCheckBox::indicator:disabled { background: @alt@; border-color: @border@; }
QCheckBox::indicator:checked:disabled { background: @accentSoft@; border-color: transparent; }
QRadioButton::indicator { width: 16px; height: 16px; border: 1px solid @borderStrong@; border-radius: 9px;
    background: @card@; }
QRadioButton::indicator:checked { background: @accent@; border-color: @accent@; image: @dot@; }

/* ---------- кнопки ---------- */
/* Обычная кнопка спокойная: рамка и лёгкая заливка под курсором. Цвет — только у главного действия. */
QPushButton { background: @card@; color: @text@; border: 1px solid @borderStrong@; border-radius: 7px;
              padding: 5px 14px; min-height: 18px; }
QPushButton:hover { background: @alt@; }
QPushButton:pressed { background: @border@; }
QPushButton:disabled { color: @disabled@; background: @alt@; border-color: @border@; }
QPushButton:checked { background: @accentSoft@; border-color: @accent@; }

QPushButton[role="primary"], QPushButton#btnPrimary, QPushButton#readBoardParameters, QPushButton#btnWizard {
    background: @accent@; color: #ffffff; border: 1px solid @accent@; font-weight: 600; }
QPushButton[role="primary"]:hover, QPushButton#btnPrimary:hover, QPushButton#readBoardParameters:hover,
QPushButton#btnWizard:hover { background: @accentHover@; border-color: @accentHover@; }
QPushButton[role="primary"]:pressed, QPushButton#btnPrimary:pressed, QPushButton#readBoardParameters:pressed,
QPushButton#btnWizard:pressed { background: @accentPressed@; border-color: @accentPressed@; }
QPushButton[role="primary"]:disabled, QPushButton#btnPrimary:disabled, QPushButton#readBoardParameters:disabled,
QPushButton#btnWizard:disabled { background: @alt@; color: @disabled@; border-color: @border@; font-weight: 600; }

QPushButton[role="success"], QPushButton#btnStart {
    background: @success@; color: #ffffff; border: 1px solid @success@; font-weight: 600; }
QPushButton[role="success"]:hover, QPushButton#btnStart:hover { background: @successHover@; border-color: @successHover@; }
QPushButton[role="success"]:disabled, QPushButton#btnStart:disabled {
    background: @alt@; color: @disabled@; border-color: @border@; }

QPushButton[role="danger"], QPushButton#btnStop {
    background: @danger@; color: #ffffff; border: 1px solid @danger@; font-weight: 600; }
QPushButton[role="danger"]:hover, QPushButton#btnStop:hover { background: @dangerHover@; border-color: @dangerHover@; }
QPushButton[role="danger"]:disabled, QPushButton#btnStop:disabled {
    background: @alt@; color: @disabled@; border-color: @border@; }

QPushButton#btnPause, QPushButton#btnRetry { background: @warnSoft@; color: @warnText@; border: 1px solid transparent;
                                             font-weight: 600; }
QPushButton#btnPause:hover, QPushButton#btnRetry:hover { border-color: @warn@; }
QPushButton#btnPause:disabled { background: @alt@; color: @disabled@; border-color: @border@; }
QPushButton#btnResume { background: @accent@; color: #ffffff; border: 1px solid @accent@; font-weight: 600; }
QPushButton#btnResume:disabled { background: @alt@; color: @disabled@; border-color: @border@; }
QPushButton#btnTheme { background: @accent@; color: #ffffff; border: 1px solid @accent@; font-weight: 600; }

QPushButton#btnThemeToggle { background: transparent; color: @muted@; border: 1px solid transparent; border-radius: 7px;
                             padding: 4px 10px; min-height: 16px; margin: 6px 4px 6px 0px; }
QPushButton#btnThemeToggle:hover { color: @text@; background: @hoverFill@; }

/* ---------- подписи ---------- */
QLabel[role="caption"] { color: @muted@; font-size: 12px; }
QLabel[role="hint"] { color: @muted@; }
QLabel[role="status"] { background: @alt@; color: @text@; border-radius: 7px; padding: 6px 10px; }
QLabel[role="status"][tone="info"] { background: @accentSoft@; color: @accentText@; }
QLabel[role="status"][tone="ok"] { background: @okSoft@; color: @okText@; }
QLabel[role="status"][tone="warn"] { background: @warnSoft@; color: @warnText@; }
QLabel[role="status"][tone="error"] { background: @errorSoft@; color: @errorText@; }
QLabel#boardIpAddress { background: @accentSoft@; color: @accentText@; border-radius: 8px; padding: 6px 12px;
                        font-weight: 700; }
QLabel#boardParametersStatus { color: @muted@; }
QLabel#hintLabel { color: @muted@; }

/* ---------- плитки ---------- */
QFrame[role="tile"] { background: @alt@; border: 1px solid transparent; border-radius: 8px; }
QFrame[role="tile"][tone="info"] { background: @accentSoft@; border-color: transparent; }
QFrame[role="tile"][tone="ok"] { background: @okSoft@; border-color: transparent; }
QFrame[role="tile"][tone="warn"] { background: @warnSoft@; border-color: transparent; }
QFrame[role="tile"][tone="error"] { background: @errorSoft@; border-color: transparent; }
QLabel[role="tileCaption"] { color: @muted@; font-size: 11px; }

/* ---------- таблицы полей модулей: шапка, строки через одну, черта между строками ---------- */
QFrame[role="gridHeader"] { background: @alt@; border: none; border-bottom: 1px solid @borderStrong@;
                            border-top-left-radius: 6px; border-top-right-radius: 6px; }
QFrame[role="gridRow"] { background: transparent; border: none; border-bottom: 1px solid @border@; }
QFrame[role="gridStripe"] { background: @hoverFill@; border: none; border-bottom: 1px solid @border@; }
QLabel[role="tileValue"] { font-size: 18px; font-weight: 600; }

/* ---------- таблицы и списки ---------- */
QTableView, QTreeView, QListView, QListWidget { background: @card@; alternate-background-color: @alt@;
    border: 1px solid @border@; border-radius: 7px; gridline-color: @border@;
    selection-background-color: @accentSoft@; selection-color: @text@; outline: 0px; }
QHeaderView { background: transparent; }
QHeaderView::section { background: @alt@; color: @muted@; border: none; border-bottom: 1px solid @border@;
                       padding: 6px 10px; font-weight: 600; font-size: 12px; }
QTableCornerButton::section { background: @alt@; border: none; }
QTableView::item { padding: 2px 6px; }
QListWidget { padding: 4px; }
QListWidget::item { padding: 8px 10px; border-radius: 8px; }
QListWidget::item:selected { background: @accentSoft@; color: @accentText@; font-weight: 600; }

/* ---------- прогресс, прокрутка, разделители ---------- */
QProgressBar { border: none; border-radius: 6px; background: @alt@; text-align: center; min-height: 14px;
               color: @muted@; font-size: 11px; }
QProgressBar::chunk { background: @accent@; border-radius: 6px; }
QScrollArea { background: transparent; border: none; }
QScrollArea > QWidget > QWidget { background: transparent; }
QScrollBar:vertical { background: transparent; width: 12px; margin: 2px; }
QScrollBar:horizontal { background: transparent; height: 12px; margin: 2px; }
QScrollBar::handle:vertical { background: @handle@; border-radius: 4px; min-height: 30px; }
QScrollBar::handle:horizontal { background: @handle@; border-radius: 4px; min-width: 30px; }
QScrollBar::handle:vertical:hover, QScrollBar::handle:horizontal:hover { background: @handleHover@; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0px; height: 0px; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QSplitter::handle { background: transparent; }
QSplitter::handle:horizontal { width: 8px; }
QSplitter::handle:vertical { height: 8px; }

/* ---------- полоса состояния ---------- */
QStatusBar { background: @window@; border-top: 1px solid @border@; }
QStatusBar::item { border: none; }
QStatusBar QLabel { color: @muted@; padding: 0px 8px; }

/* ---------- прежние окошки калибровки ---------- */
QListWidget#scpiErrorQueue { background: @errorSoft@; border: 1px solid transparent; }
QPlainTextEdit#boardParametersOutput, QPlainTextEdit#boardParametersDetails { font-size: 12px; padding: 10px; }
)CSS");

    const std::vector<std::pair<const char*, const char*>> map = {
        {"@window@", t.window},       {"@card@", t.card},           {"@alt@", t.alt},
        {"@borderStrong@", t.borderStrong}, {"@border@", t.border},   {"@text@", t.text},
        {"@muted@", t.muted},         {"@disabled@", t.disabled},   {"@accentHover@", t.accentHover},
        {"@accentPressed@", t.accentPressed}, {"@accentSoft@", t.accentSoft}, {"@accentText@", t.accentText},
        {"@accent@", t.accent},       {"@successHover@", t.successHover}, {"@success@", t.success},
        {"@dangerHover@", t.dangerHover}, {"@danger@", t.danger},   {"@warnSoft@", t.warnSoft},
        {"@warnText@", t.warnText},   {"@warn@", t.warn},           {"@okSoft@", t.okSoft},
        {"@okText@", t.okText},       {"@errorSoft@", t.errorSoft}, {"@errorText@", t.errorText},
        {"@hoverFill@", t.hoverFill}, {"@handleHover@", t.handleHover}, {"@handle@", t.handle}};
    for (const auto& [token, value] : map) {
        css.replace(QLatin1String(token), QLatin1String(value));
    }
    const QString none = QStringLiteral("none");
    const QString check = iconUrl(QStringLiteral("check"));
    const QString dot = iconUrl(QStringLiteral("dot"));
    css.replace(QStringLiteral("@check@"), check.isEmpty() ? none : check);
    css.replace(QStringLiteral("@dot@"), dot.isEmpty() ? none : dot);
    return css;
}

}  // namespace

AppTheme currentAppTheme()
{
    return g_theme;
}

AppTheme loadAppTheme()
{
    QSettings settings;
    const QString v = settings.value(QStringLiteral("ui/theme"), QStringLiteral("light")).toString();
    return v == QLatin1String("dark") ? AppTheme::Dark : AppTheme::Light;
}

void saveAppTheme(AppTheme theme)
{
    QSettings settings;
    settings.setValue(QStringLiteral("ui/theme"),
                      theme == AppTheme::Dark ? QStringLiteral("dark") : QStringLiteral("light"));
}

void applyAppTheme(QApplication& app, AppTheme theme)
{
    g_theme = theme;
    if (auto* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        app.setStyle(fusion);
    }

    const Tokens& t = theme == AppTheme::Dark ? kDark : kLight;
    QPalette pal;
    pal.setColor(QPalette::Window, QColor(QLatin1String(t.window)));
    pal.setColor(QPalette::WindowText, QColor(QLatin1String(t.text)));
    pal.setColor(QPalette::Base, QColor(QLatin1String(t.card)));
    pal.setColor(QPalette::AlternateBase, QColor(QLatin1String(t.alt)));
    pal.setColor(QPalette::Text, QColor(QLatin1String(t.text)));
    pal.setColor(QPalette::Button, QColor(QLatin1String(t.card)));
    pal.setColor(QPalette::ButtonText, QColor(QLatin1String(t.text)));
    pal.setColor(QPalette::Highlight, QColor(QLatin1String(t.accent)));
    pal.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#ffffff")));
    pal.setColor(QPalette::ToolTipBase, QColor(QLatin1String(t.card)));
    pal.setColor(QPalette::ToolTipText, QColor(QLatin1String(t.text)));
    pal.setColor(QPalette::PlaceholderText, QColor(QLatin1String(t.muted)));
    pal.setColor(QPalette::Mid, QColor(QLatin1String(t.border)));
    pal.setColor(QPalette::Light, QColor(QLatin1String(t.card)));
    pal.setColor(QPalette::Dark, QColor(QLatin1String(t.borderStrong)));
    pal.setColor(QPalette::Disabled, QPalette::Text, QColor(QLatin1String(t.disabled)));
    pal.setColor(QPalette::Disabled, QPalette::WindowText, QColor(QLatin1String(t.disabled)));
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(QLatin1String(t.disabled)));
    app.setPalette(pal);
    app.setStyleSheet(appStyleSheet(theme));
}

QString appStyleSheet(AppTheme theme)
{
    return build(theme == AppTheme::Dark ? kDark : kLight);
}

QString matrixCellStyle(int status, AppTheme theme)
{
    // Ячейки матрицы — те же мягкие тона, что у «таблеток» состояния.
    const Tokens& t = theme == AppTheme::Dark ? kDark : kLight;
    const auto cell = [](const char* bg, const char* fg) {
        return QStringLiteral("background:%1; color:%2;").arg(QLatin1String(bg), QLatin1String(fg));
    };
    switch (status) {
    case 1: return cell(t.accentSoft, t.accentText);
    case 2: return cell(t.okSoft, t.okText);
    case 3: return cell(t.warnSoft, t.warnText);
    case 4: return cell(t.errorSoft, t.errorText);
    case -1: return cell(t.window, t.disabled);
    default: return cell(t.alt, t.muted);
    }
}

QColor themeColor(ThemeColor role)
{
    const Tokens& t = tokens();
    switch (role) {
    case ThemeColor::Text: return QColor(QLatin1String(t.text));
    case ThemeColor::Muted: return QColor(QLatin1String(t.muted));
    case ThemeColor::Border: return QColor(QLatin1String(t.border));
    case ThemeColor::Accent: return QColor(QLatin1String(t.accent));
    case ThemeColor::OkText: return QColor(QLatin1String(t.okText));
    case ThemeColor::WarnText: return QColor(QLatin1String(t.warnText));
    case ThemeColor::ErrorText: return QColor(QLatin1String(t.errorText));
    case ThemeColor::Ok: return QColor(QLatin1String(t.success));
    case ThemeColor::Warn: return QColor(QLatin1String(t.warn));
    case ThemeColor::Error: return QColor(QLatin1String(t.danger));
    }
    return QColor(QLatin1String(t.text));
}

QColor seriesColor(int index)
{
    const auto& set = g_theme == AppTheme::Dark ? kSeriesDark : kSeriesLight;
    const int n = static_cast<int>(std::size(set));
    return QColor(QLatin1String(set[((index % n) + n) % n]));
}
