#pragma once

#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace afar::report {

inline bool replaceFile(const std::filesystem::path& temporary,
                        const std::filesystem::path& destination,
                        std::string& diagnostics)
{
#ifdef _WIN32
    if (MoveFileExW(temporary.c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    diagnostics = "cannot commit file: "
        + std::system_category().message(static_cast<int>(GetLastError()));
    return false;
#else
    std::error_code ec;
    std::filesystem::rename(temporary, destination, ec);
    if (!ec) {
        return true;
    }
    diagnostics = "cannot commit file: " + ec.message();
    return false;
#endif
}

}  // namespace afar::report
