#pragma once

#include <QColor>
#include <QString>

class QApplication;

enum class AppTheme { Light, Dark };

/// Цвета текущей темы для того, что рисуется вручную (графики, ячейки таблиц).
enum class ThemeColor { Text, Muted, Border, Accent, Ok, Warn, Error, OkText, WarnText, ErrorText };

[[nodiscard]] AppTheme currentAppTheme();
[[nodiscard]] AppTheme loadAppTheme();
void saveAppTheme(AppTheme theme);
void applyAppTheme(QApplication& app, AppTheme theme);
[[nodiscard]] QString appStyleSheet(AppTheme theme);
[[nodiscard]] QString matrixCellStyle(int status, AppTheme theme);

/// Цвет из палитры текущей темы.
[[nodiscard]] QColor themeColor(ThemeColor role);
/// Цвет ряда графика: согласованные тона, i-й ряд, по кругу.
[[nodiscard]] QColor seriesColor(int index);
