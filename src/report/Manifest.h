#pragma once

#include "SeriesDirectory.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace afar::report {

/// SHA-256 файла (hex lowercase, 64 символа). Без OpenSSL — встроенная реализация.
[[nodiscard]] std::string sha256FileHex(const std::filesystem::path& path,
                                        std::string& diagnostics);

[[nodiscard]] std::string sha256BytesHex(const void* data, std::size_t size);

struct ManifestEntry {
    std::string hex;
    std::string filename;
};

/// Пишет `manifest.sha256` (формат sha256sum) по всем файлам серии, кроме самого манифеста.
bool writeManifest(const SeriesDirectory& series, std::string& diagnostics);

/// Читает манифест и проверяет, что хеши совпадают с пересчётом по файлам.
bool verifyManifest(const SeriesDirectory& series, std::string& diagnostics);

bool readManifestFile(const std::filesystem::path& path,
                      std::vector<ManifestEntry>& out,
                      std::string& diagnostics);

}  // namespace afar::report
