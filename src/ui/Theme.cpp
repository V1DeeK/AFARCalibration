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

constexpr Tokens kLight{
    "#f5f5f7", "#ffffff", "#f4f4f7", "#dcdce1", "#c4c4cb", "#1d1d1f", "#6e6e73", "#a1a1a6",
    "#007aff", "#006ae0", "#0058bd", "rgba(0,122,255,0.14)", "#0a5fc4",
    "#34c759", "#2cb04f", "#ff3b30", "#e5352b", "#ff9f0a",
    "rgba(52,199,89,0.18)", "#1b7a38", "rgba(255,159,10,0.20)", "#8a5300",
    "rgba(255,59,48,0.14)", "#c4281f",
    "rgba(120,120,128,0.10)", "rgba(120,120,128,0.40)", "rgba(120,120,128,0.62)"};

constexpr Tokens kDark{
    "#1c1c1e", "#2c2c2e", "#343437", "#434347", "#58585d", "#f2f2f7", "#9a9aa1", "#69696e",
    "#0a84ff", "#3395ff", "#006fe0", "rgba(10,132,255,0.30)", "#7ab8ff",
    "#30d158", "#4ade6f", "#ff453a", "#ff6259", "#ff9f0a",
    "rgba(48,209,88,0.24)", "#6fe08d", "rgba(255,159,10,0.26)", "#ffc266",
    "rgba(255,69,58,0.26)", "#ff8a82",
    "rgba(235,235,245,0.10)", "rgba(235,235,245,0.30)", "rgba(235,235,245,0.52)"};

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
QWidget:disabled { color: @disabled@; }
QMainWindow, QDialog, QWizard, QWizardPage { background: @window@; }
QToolTip { background: @card@; color: @text@; border: 1px solid @border@; border-radius: 6px; padding: 4px 8px; }

/* ---------- окошки-карточки ---------- */
QGroupBox { background: @card@; border: 1px solid @border@; border-radius: 12px;
            margin-top: 0px; padding: 38px 14px 14px 14px; }
QGroupBox::title { subcontrol-origin: padding; subcontrol-position: top left; left: 14px; top: 11px;
                   padding: 0px; color: @text@; font-weight: 700; }
QGroupBox::indicator { width: 16px; height: 16px; border: 1px solid @borderStrong@; border-radius: 4px;
                       background: @card@; }
QGroupBox::indicator:checked { background: @accent@; border-color: @accent@; image: @check@; }
QFrame#ConnectionBar { background: @card@; border: 1px solid @border@; border-radius: 12px; }

/* ---------- вкладки ---------- */
QTabWidget::pane { border: 1px solid @border@; border-radius: 12px; top: -1px; background: @card@; }
QTabBar::tab { background: transparent; color: @muted@; padding: 7px 16px; margin-right: 2px;
               border: 1px solid transparent; border-bottom: none;
               border-top-left-radius: 9px; border-top-right-radius: 9px; }
QTabBar::tab:hover:!selected { color: @text@; background: @hoverFill@; }
QTabBar::tab:selected { background: @card@; color: @text@; font-weight: 600; border-color: @border@; }
QTabBar::tab:disabled { color: @disabled@; }
QTabBar#mainTabBar { background: transparent; }
QTabBar#mainTabBar::tab { padding: 8px 18px; margin: 6px 3px 6px 0px; border: 1px solid transparent;
                          border-radius: 9px; }
QTabBar#mainTabBar::tab:selected { background: @accent@; color: #ffffff; font-weight: 600; border-color: @accent@; }
QTabBar#mainTabBar::tab:hover:!selected { background: @hoverFill@; color: @text@; }
/* Стрелки прокрутки у вкладок «Модулей» в узком окне — во всю свою ширину. */
QTabBar::scroller { width: 64px; }
QTabWidget#mainTabs::pane { border: none; background: transparent; border-radius: 0px; top: 0px; }
QTabWidget#mainTabs::tab-bar { left: 12px; }
QTabWidget#calibrationTabs QTabBar::tab, QTabWidget#vnaTabs QTabBar::tab { padding: 6px 7px; margin-right: 0px; font-size: 12px; }

/* ---------- поля ввода ---------- */
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QPlainTextEdit, QTextEdit {
    background: @card@; border: 1px solid @border@; border-radius: 8px; padding: 4px 10px; min-height: 20px;
    selection-background-color: @accent@; selection-color: #ffffff; }
QPlainTextEdit, QTextEdit { padding: 6px 8px; }
QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover, QComboBox:hover { border-color: @borderStrong@; }
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
QPushButton { background: @card@; color: @text@; border: 1px solid @borderStrong@; border-radius: 8px;
              padding: 5px 14px; min-height: 18px; }
QPushButton:hover { border-color: @accent@; }
QPushButton:pressed { background: @alt@; }
QPushButton:disabled { color: @disabled@; background: @alt@; border-color: @border@; }
QPushButton:checked { background: @accentSoft@; border-color: @accent@; }

QPushButton[role="primary"], QPushButton#btnPrimary, QPushButton#readBoardParameters, QPushButton#btnWizard {
    background: @accent@; color: #ffffff; border: 1px solid @accent@; font-weight: 600; }
QPushButton[role="primary"]:hover, QPushButton#btnPrimary:hover, QPushButton#readBoardParameters:hover,
QPushButton#btnWizard:hover { background: @accentHover@; border-color: @accentHover@; }
QPushButton[role="primary"]:pressed, QPushButton#btnPrimary:pressed, QPushButton#readBoardParameters:pressed,
QPushButton#btnWizard:pressed { background: @accentPressed@; border-color: @accentPressed@; }
QPushButton[role="primary"]:disabled, QPushButton#btnPrimary:disabled, QPushButton#readBoardParameters:disabled,
QPushButton#btnWizard:disabled { background: @accentSoft@; color: @accentText@; border-color: transparent; }

QPushButton[role="success"], QPushButton#btnStart {
    background: @success@; color: #ffffff; border: 1px solid @success@; font-weight: 600; }
QPushButton[role="success"]:hover, QPushButton#btnStart:hover { background: @successHover@; border-color: @successHover@; }
QPushButton[role="success"]:disabled, QPushButton#btnStart:disabled {
    background: @okSoft@; color: @okText@; border-color: transparent; }

QPushButton[role="danger"], QPushButton#btnStop {
    background: @danger@; color: #ffffff; border: 1px solid @danger@; font-weight: 600; }
QPushButton[role="danger"]:hover, QPushButton#btnStop:hover { background: @dangerHover@; border-color: @dangerHover@; }
QPushButton[role="danger"]:disabled, QPushButton#btnStop:disabled {
    background: @errorSoft@; color: @errorText@; border-color: transparent; }

QPushButton#btnPause, QPushButton#btnRetry { background: @warn@; color: #1d1d1f; border: 1px solid @warn@; font-weight: 600; }
QPushButton#btnPause:disabled { background: @warnSoft@; color: @warnText@; border-color: transparent; }
QPushButton#btnResume { background: #5e5ce6; color: #ffffff; border: 1px solid #5e5ce6; font-weight: 600; }
QPushButton#btnResume:disabled { background: rgba(94,92,230,0.22); color: #7d7bf0; border-color: transparent; }
QPushButton#btnTheme { background: @accent@; color: #ffffff; border: 1px solid @accent@; font-weight: 600; }

QPushButton#btnThemeToggle { background: transparent; border: 1px solid @border@; border-radius: 8px;
                             padding: 3px 12px; min-height: 16px; margin: 6px 4px 6px 0px; }
QPushButton#btnThemeToggle:hover { border-color: @accent@; background: @hoverFill@; }

/* ---------- подписи ---------- */
QLabel[role="caption"] { color: @muted@; font-size: 12px; }
QLabel[role="hint"] { color: @muted@; }
QLabel[role="status"] { background: @alt@; color: @text@; border-radius: 8px; padding: 6px 10px; }
QLabel[role="status"][tone="info"] { background: @accentSoft@; color: @accentText@; }
QLabel[role="status"][tone="ok"] { background: @okSoft@; color: @okText@; }
QLabel[role="status"][tone="warn"] { background: @warnSoft@; color: @warnText@; }
QLabel[role="status"][tone="error"] { background: @errorSoft@; color: @errorText@; }
QLabel#boardIpAddress { background: @accentSoft@; color: @accentText@; border-radius: 8px; padding: 6px 12px;
                        font-weight: 700; }
QLabel#boardParametersStatus { color: @muted@; }
QLabel#hintLabel { color: @muted@; }

/* ---------- плитки ---------- */
QFrame[role="tile"] { background: @alt@; border: 1px solid @border@; border-radius: 9px; }
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
QLabel[role="tileValue"] { font-size: 18px; font-weight: 700; }

/* ---------- таблицы и списки ---------- */
QTableView, QTreeView, QListView, QListWidget { background: @card@; alternate-background-color: @alt@;
    border: 1px solid @border@; border-radius: 8px; gridline-color: @border@;
    selection-background-color: @accentSoft@; selection-color: @text@; outline: 0px; }
QHeaderView { background: transparent; }
QHeaderView::section { background: @alt@; color: @muted@; border: none; border-bottom: 1px solid @border@;
                       padding: 6px 10px; font-weight: 600; }
QTableCornerButton::section { background: @alt@; border: none; }
QTableView::item { padding: 2px 6px; }
QListWidget { padding: 4px; }
QListWidget::item { padding: 8px 10px; border-radius: 8px; }
QListWidget::item:selected { background: @accent@; color: #ffffff; }

/* ---------- прогресс, прокрутка, разделители ---------- */
QProgressBar { border: 1px solid @border@; border-radius: 8px; background: @card@; text-align: center; min-height: 16px; }
QProgressBar::chunk { background: @accent@; border-radius: 7px; }
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
    const bool dark = theme == AppTheme::Dark;
    switch (status) {
    case 1:
        return dark ? QStringLiteral("background:#1e3a5f; color:#7dc1ff;")
                    : QStringLiteral("background:#cfe2ff; color:#084298;");
    case 2:
        return dark ? QStringLiteral("background:#16351f; color:#7dffa6;")
                    : QStringLiteral("background:#d1e7dd; color:#0f5132;");
    case 3:
        return dark ? QStringLiteral("background:#3d2e00; color:#ffd60a;")
                    : QStringLiteral("background:#fff3cd; color:#664d03;");
    case 4:
        return dark ? QStringLiteral("background:#3d1010; color:#ff8a80;")
                    : QStringLiteral("background:#f8d7da; color:#842029;");
    case -1:
        return dark ? QStringLiteral("background:#2c2c2e; color:#636366;")
                    : QStringLiteral("background:#f1f3f5; color:#adb5bd;");
    default:
        return dark ? QStringLiteral("background:#3a3a3c; color:#f2f2f7;")
                    : QStringLiteral("background:#e9ecef; color:#495057;");
    }
}
