#include "ProcessMetrics.h"

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <psapi.h>
#  include <tlhelp32.h>
#elif defined(__APPLE__)
#  include <libproc.h>
#  include <mach/mach.h>
#  include <unistd.h>
#  include <vector>
#else
#  include <dirent.h>
#  include <fstream>
#  include <string>
#  include <unistd.h>
#endif

namespace afar {

ProcessMetrics current_process_metrics() noexcept
{
    ProcessMetrics result;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                             sizeof(memory))) {
        result.rss_bytes = memory.WorkingSetSize;
        result.private_bytes = memory.PrivateUsage;
    }
    DWORD handles = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &handles)) {
        result.handle_count = handles;
    }
    const DWORD processId = GetCurrentProcessId();
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        if (Thread32First(snapshot, &entry)) {
            do {
                if (entry.th32OwnerProcessID == processId) {
                    ++result.thread_count;
                }
            } while (Thread32Next(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count)
        == KERN_SUCCESS) {
        result.rss_bytes = info.resident_size;
    }
    task_vm_info_data_t vm{};
    mach_msg_type_number_t vmCount = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm), &vmCount)
        == KERN_SUCCESS) {
        result.private_bytes = vm.phys_footprint;
    }
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t threadCount = 0;
    if (task_threads(mach_task_self(), &threads, &threadCount) == KERN_SUCCESS) {
        result.thread_count = threadCount;
        for (mach_msg_type_number_t i = 0; i < threadCount; ++i) {
            mach_port_deallocate(mach_task_self(), threads[i]);
        }
        vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads),
                      threadCount * sizeof(thread_act_t));
    }
    const int bytes = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, nullptr, 0);
    if (bytes > 0) {
        result.handle_count = static_cast<std::uint32_t>(
            static_cast<std::size_t>(bytes) / sizeof(proc_fdinfo));
    }
#else
    const auto pageSize = static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
    {
        std::ifstream statm("/proc/self/statm");
        std::uint64_t totalPages = 0;
        std::uint64_t residentPages = 0;
        if (statm >> totalPages >> residentPages) {
            result.rss_bytes = residentPages * pageSize;
        }
    }
    {
        std::ifstream status("/proc/self/status");
        std::string key;
        while (status >> key) {
            if (key == "RssAnon:") {
                std::uint64_t kb = 0;
                status >> kb;
                result.private_bytes = kb * 1024u;
            } else if (key == "Threads:") {
                std::uint32_t threads = 0;
                status >> threads;
                result.thread_count = threads;
            }
            status.ignore(4096, '\n');
        }
    }
    if (DIR* dir = ::opendir("/proc/self/fd")) {
        std::uint32_t fds = 0;
        while (const dirent* entry = ::readdir(dir)) {
            if (entry->d_name[0] != '.') {
                ++fds;
            }
        }
        ::closedir(dir);
        // Минус дескриптор самого opendir.
        result.handle_count = fds > 0 ? fds - 1 : 0;
    }
#endif
    return result;
}

}  // namespace afar
