#pragma once

#include <filesystem>
#include <string>

// Пользовательские данные Spectrum хранятся отдельно от AFARCalibration.

// Каталог, где лежит сам исполняемый файл.
std::filesystem::path executable_dir();

// Documents/AFAR/Spectrum (или эквивалент системной папки Documents).
std::filesystem::path spectrum_data_dir();

// Абсолютный путь берётся как есть, относительный — внутри spectrum_data_dir().
// Комплектные rx_calibration*.csv читаются из data/spectrum рядом с EXE.
std::filesystem::path resolve_data_path(const std::string& path);
