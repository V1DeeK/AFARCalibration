#include "paths.hpp"

// windows.h тянет за собой макросы min/max, ломающие std::min/std::max в любом
// файле, который его увидит, — поэтому он заперт в этом .cpp, а не в заголовке.
#ifdef _WIN32
// оба макроса mingw-овый libstdc++ уже задаёт сам (os_defines.h, тянется из
// <filesystem> в paths.hpp) — переопределение здесь ловится как -Werror
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#else
#include <cstdlib>
#include <limits.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

fs::path executable_dir() {
#ifdef _WIN32
    char  buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return fs::current_path();
    return fs::path(std::string(buf, len)).parent_path();
#else
    char    buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return fs::current_path();
    return fs::path(std::string(buf, len)).parent_path();
#endif
}

fs::path spectrum_data_dir() {
#ifdef _WIN32
    PWSTR documents = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documents))) {
        fs::path result(documents);
        CoTaskMemFree(documents);
        return result / L"AFAR" / L"Spectrum";
    }
#else
    if (const char* home = std::getenv("HOME"))
        return fs::path(home) / "Documents" / "AFAR" / "Spectrum";
#endif
    return executable_dir() / "user-data" / "spectrum";
}

fs::path resolve_data_path(const std::string& path) {
    fs::path p(path);
    if (!p.is_absolute()) {
        if (!p.empty() && *p.begin() == "data")
            p = p.lexically_relative("data");

        const std::string filename = p.filename().string();
        if (filename.rfind("rx_calibration", 0) == 0) {
            const fs::path bundled = executable_dir() / "data" / "spectrum" / p;
            std::error_code existsError;
            if (fs::exists(bundled, existsError))
                return bundled;
        }
        p = spectrum_data_dir() / p;
    }

    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    return p;
}
