#pragma once

#include <QString>

class QApplication;

enum class AppTheme { Light, Dark };

[[nodiscard]] AppTheme currentAppTheme();
[[nodiscard]] AppTheme loadAppTheme();
void saveAppTheme(AppTheme theme);
void applyAppTheme(QApplication& app, AppTheme theme);
[[nodiscard]] QString appStyleSheet(AppTheme theme);
[[nodiscard]] QString matrixCellStyle(int status, AppTheme theme);
