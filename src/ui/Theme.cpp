#include "Theme.h"

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QSettings>
#include <QStyleFactory>

namespace {

AppTheme g_theme = AppTheme::Light;

QString macosBase(AppTheme theme)
{
    const bool dark = theme == AppTheme::Dark;
    const char* window = dark ? "#1c1c1e" : "#f5f5f7";
    const char* card = dark ? "#2c2c2e" : "#ffffff";
    const char* text = dark ? "#f2f2f7" : "#1d1d1f";
    const char* muted = dark ? "#8e8e93" : "#6e6e73";
    const char* line = dark ? "#3a3a3c" : "#d2d2d7";
    const char* accent = dark ? "#0a84ff" : "#007aff";
    const char* selText = "#ffffff";

    return QStringLiteral(
               "QWidget { font-family: 'Segoe UI', 'SF Pro Display', 'Helvetica Neue', sans-serif; "
               "font-size: 13px; color: %1; }"
               "QMainWindow, QDialog, QWizard, QWizardPage { background: %2; }"
               "QFrame#ConnectionBar { background: %3; border: 1px solid %4; border-radius: 12px; }"
               "QGroupBox { background: %3; border: 1px solid %4; border-radius: 12px; "
               "margin-top: 12px; padding: 10px 8px 8px 8px; font-weight: 600; }"
               "QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 6px; color: %1; }"
               "QTabWidget::pane { border: 1px solid %4; border-radius: 12px; top: -1px; background: %3; }"
               "QTabBar::tab { background: transparent; color: %5; padding: 8px 16px; "
               "margin-right: 2px; border-top-left-radius: 10px; border-top-right-radius: 10px; }"
               "QTabBar::tab:selected { background: %3; color: %1; font-weight: 600; }"
               "QListWidget { background: %3; border: 1px solid %4; border-radius: 12px; padding: 4px; }"
               "QListWidget::item { padding: 8px 10px; border-radius: 8px; }"
               "QListWidget::item:selected { background: %6; color: %7; }"
               "QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox { background: %3; border: 1px solid %4; "
               "border-radius: 8px; padding: 4px 8px; min-height: 24px; }"
               "QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus { border: 2px solid %6; }"
               "QProgressBar { border: 1px solid %4; border-radius: 8px; background: %3; text-align: center; "
               "min-height: 16px; }"
               "QProgressBar::chunk { background: %6; border-radius: 7px; }"
               "QLabel#hintLabel { color: %5; }"
               "QPushButton { border-radius: 10px; padding: 6px 14px; min-height: 32px; "
               "background: %3; border: 1px solid %4; }"
               "QPushButton:hover { border-color: %6; }"
               "QPushButton#btnWizard { background: %6; color: %7; font-weight: 700; border: none; min-width: 160px; }"
               "QPushButton#btnWizard:disabled { background: #80bfff; color: #00325c; }"
               "QPushButton#btnStart { background: #34c759; color: #ffffff; font-weight: 700; border: none; min-width: 120px; }"
               "QPushButton#btnStart:disabled { background: #8ee0a4; color: #14532d; }"
               "QPushButton#btnPause { background: #ff9f0a; color: #1d1d1f; font-weight: 700; border: none; min-width: 120px; }"
               "QPushButton#btnPause:disabled { background: #ffd699; color: #7a4d00; }"
               "QPushButton#btnStop { background: #ff3b30; color: #ffffff; font-weight: 700; border: none; min-width: 120px; }"
               "QPushButton#btnStop:disabled { background: #ff9b96; color: #5c1010; }"
               "QPushButton#btnTheme { background: %6; color: %7; font-weight: 600; border: none; min-width: 132px; }"
               "QPushButton#btnRetry { background: #ff9f0a; color: #1d1d1f; font-weight: 600; border: none; }"
               "QPushButton#btnResume { background: #5e5ce6; color: #ffffff; font-weight: 700; border: none; min-height: 40px; }"
               "QPushButton#btnResume:disabled { background: #a5a3f5; color: #2a2870; }"
               "QPushButton#btnPrimary { background: %6; color: %7; font-weight: 700; border: none; min-height: 40px; }")
        .arg(QLatin1String(text), QLatin1String(window), QLatin1String(card), QLatin1String(line),
             QLatin1String(muted), QLatin1String(accent), QLatin1String(selText));
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

    QPalette pal;
    if (theme == AppTheme::Dark) {
        pal.setColor(QPalette::Window, QColor(QStringLiteral("#1c1c1e")));
        pal.setColor(QPalette::WindowText, QColor(QStringLiteral("#f2f2f7")));
        pal.setColor(QPalette::Base, QColor(QStringLiteral("#2c2c2e")));
        pal.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#3a3a3c")));
        pal.setColor(QPalette::Text, QColor(QStringLiteral("#f2f2f7")));
        pal.setColor(QPalette::Button, QColor(QStringLiteral("#3a3a3c")));
        pal.setColor(QPalette::ButtonText, QColor(QStringLiteral("#f2f2f7")));
        pal.setColor(QPalette::Highlight, QColor(QStringLiteral("#0a84ff")));
        pal.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#ffffff")));
        pal.setColor(QPalette::ToolTipBase, QColor(QStringLiteral("#2c2c2e")));
        pal.setColor(QPalette::ToolTipText, QColor(QStringLiteral("#f2f2f7")));
        pal.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#8e8e93")));
        pal.setColor(QPalette::Mid, QColor(QStringLiteral("#636366")));
        pal.setColor(QPalette::Light, QColor(QStringLiteral("#48484a")));
        pal.setColor(QPalette::Dark, QColor(QStringLiteral("#0d0d0d")));
    } else {
        pal.setColor(QPalette::Window, QColor(QStringLiteral("#f5f5f7")));
        pal.setColor(QPalette::WindowText, QColor(QStringLiteral("#1d1d1f")));
        pal.setColor(QPalette::Base, QColor(QStringLiteral("#ffffff")));
        pal.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#f2f2f7")));
        pal.setColor(QPalette::Text, QColor(QStringLiteral("#1d1d1f")));
        pal.setColor(QPalette::Button, QColor(QStringLiteral("#ffffff")));
        pal.setColor(QPalette::ButtonText, QColor(QStringLiteral("#1d1d1f")));
        pal.setColor(QPalette::Highlight, QColor(QStringLiteral("#007aff")));
        pal.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#ffffff")));
        pal.setColor(QPalette::ToolTipBase, QColor(QStringLiteral("#ffffff")));
        pal.setColor(QPalette::ToolTipText, QColor(QStringLiteral("#1d1d1f")));
        pal.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#8e8e93")));
        pal.setColor(QPalette::Mid, QColor(QStringLiteral("#c7c7cc")));
        pal.setColor(QPalette::Light, QColor(QStringLiteral("#ffffff")));
        pal.setColor(QPalette::Dark, QColor(QStringLiteral("#8e8e93")));
    }
    app.setPalette(pal);
    app.setStyleSheet(appStyleSheet(theme));
}

QString appStyleSheet(AppTheme theme)
{
    return macosBase(theme);
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
