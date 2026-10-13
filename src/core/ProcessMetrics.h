#pragma once

#include <cstdint>

namespace afar {

/// Телеметрия собственного процесса для AT-12 (рост памяти, утечки дескрипторов).
/// Поле, которое ОС не отдаёт, остаётся 0.
struct ProcessMetrics {
    std::uint64_t rss_bytes{};      ///< резидентная память (Windows: WorkingSet)
    std::uint64_t private_bytes{};  ///< приватная память (Windows: PrivateUsage; Linux: RssAnon)
    std::uint32_t handle_count{};   ///< Windows: HANDLE; Linux/macOS: открытые дескрипторы
    std::uint32_t thread_count{};
};

ProcessMetrics current_process_metrics() noexcept;

}  // namespace afar
