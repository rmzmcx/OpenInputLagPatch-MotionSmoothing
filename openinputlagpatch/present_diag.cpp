#include <Windows.h>
#include <dwmapi.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "d3d8_hook.h"
#include "d3d9_hook.h"
#include "limiter.h"
#include "present_diag.h"

namespace {
const unsigned int present_diag_max_records = 600;

struct PresentRecord {
    uint64_t qpc;
    uint64_t interval;
    double duration_ms;
    bool duration_known;
};

PresentRecord records[present_diag_max_records];
unsigned int record_count;
unsigned int record_start;
uint64_t previous_qpc;
uint64_t first_qpc;
uint64_t total_presents;
bool hotkey_down;
HWND diagnostic_window;

__declspec(thread) uint64_t pending_duration_start;
__declspec(thread) uint64_t pending_duration_end;

struct DwmSnapshot {
    bool available;
    HRESULT result;
    uint64_t refresh_period;
    uint64_t refresh_count;
    uint64_t present_count;
    uint64_t present_confirmed;
    uint64_t refresh_confirmed;
};

DwmSnapshot query_dwm_timing() {
    DwmSnapshot snapshot = {};
    static auto get_timing = []() -> decltype(&DwmGetCompositionTimingInfo) {
        HMODULE dwmapi = LoadLibraryW(L"dwmapi.dll");
        return dwmapi
                   ? (decltype(&DwmGetCompositionTimingInfo))GetProcAddress(
                         dwmapi, "DwmGetCompositionTimingInfo")
                   : nullptr;
    }();
    if (get_timing == nullptr) {
        snapshot.result = HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        return snapshot;
    }

    DWM_TIMING_INFO timing = {};
    timing.cbSize = sizeof(timing);
    snapshot.result = get_timing(nullptr, &timing);
    snapshot.available = SUCCEEDED(snapshot.result);
    if (snapshot.available) {
        snapshot.refresh_period = timing.qpcRefreshPeriod;
        snapshot.refresh_count = timing.cRefresh;
        snapshot.present_count = timing.cDXPresent;
        snapshot.present_confirmed = timing.cDXPresentConfirmed;
        snapshot.refresh_confirmed = timing.cRefreshConfirmed;
    }
    return snapshot;
}

double qpc_ticks_to_ms(uint64_t ticks, uint64_t frequency) {
    return frequency != 0 ? (double)ticks * 1000.0 / (double)frequency : 0.0;
}

bool write_display_diag() {
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0)
        return false;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash == nullptr)
        return false;
    slash[1] = L'\0';
    wcscat_s(path, L"oilp_display_diag.txt");

    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"w") != 0 || file == nullptr)
        return false;

    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);

    HWND window = IsWindow(diagnostic_window) ? diagnostic_window : GetForegroundWindow();
    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXA monitor_info = {};
    monitor_info.cbSize = sizeof(monitor_info);
    GetMonitorInfoA(monitor, &monitor_info);

    DEVMODEA mode = {};
    mode.dmSize = sizeof(mode);
    const bool mode_ok =
        EnumDisplaySettingsA(monitor_info.szDevice[0] ? monitor_info.szDevice : nullptr,
                            ENUM_CURRENT_SETTINGS, &mode) != FALSE;
    int device_caps_refresh = 0;
    HDC dc = CreateDCA(monitor_info.szDevice[0] ? monitor_info.szDevice : nullptr,
                       nullptr, nullptr, nullptr);
    if (dc != nullptr) {
        device_caps_refresh = GetDeviceCaps(dc, VREFRESH);
        DeleteDC(dc);
    }

    const D3D8Hook::PresentInfo present = D3D8Hook::GetPresentInfo();
    const D3D9PresentInfo present9 = d3d9_get_present_info();
    const DwmSnapshot dwm = query_dwm_timing();

    fprintf(file, "oilp display pipeline diagnostics\n");
    fprintf(file, "logic fps %.2f, target presentation fps %.2f, limiter display refresh %u\n",
            Limiter::game_fps, Limiter::PresentRate(), Limiter::DisplayRefresh());
    fprintf(file, "window 0x%p, monitor %s, work area %ld,%ld-%ld,%ld\n",
            (void*)window, monitor_info.szDevice,
            monitor_info.rcWork.left, monitor_info.rcWork.top,
            monitor_info.rcWork.right, monitor_info.rcWork.bottom);
    if (mode_ok) {
        fprintf(file, "EnumDisplaySettings: %lux%lu @ %luHz, %lu bits, %luHz flags 0x%08lx\n",
                mode.dmPelsWidth, mode.dmPelsHeight, mode.dmDisplayFrequency,
                mode.dmBitsPerPel, mode.dmDisplayFlags, mode.dmFields);
    } else {
        fprintf(file, "EnumDisplaySettings failed, last error %lu\n", GetLastError());
    }
    fprintf(file, "GetDeviceCaps(VREFRESH): %d\n", device_caps_refresh);

    if (present.width != 0 || present.height != 0) {
        fprintf(file,
                "D3D8 presentation: %ux%u, back buffers %u, swap effect %u, interval 0x%08x, "
                "windowed %d\n",
                present.width, present.height, present.back_buffer_count, present.swap_effect,
                present.presentation_interval, present.windowed ? 1 : 0);
    } else {
        fprintf(file, "D3D8 presentation: no D3D8 device information\n");
    }
    if (present9.width != 0 || present9.height != 0) {
        fprintf(file,
                "D3D9 presentation: %ux%u, back buffers %u, swap effect %u, interval 0x%08x, "
                "refresh %u, windowed %d, D3D9Ex %d\n",
                present9.width, present9.height, present9.back_buffer_count,
                present9.swap_effect, present9.presentation_interval, present9.refresh_rate,
                present9.windowed ? 1 : 0, present9.d3d9ex ? 1 : 0);
    } else {
        fprintf(file, "D3D9 presentation: no D3D9 device information\n");
    }

    fprintf(file,
            "DWM timing: hr 0x%08lx, period %llu ticks, refresh %llu, submitted %llu, "
            "submitted-confirmed %llu, refresh-confirmed %llu\n",
            (unsigned long)dwm.result, (unsigned long long)dwm.refresh_period,
            (unsigned long long)dwm.refresh_count, (unsigned long long)dwm.present_count,
            (unsigned long long)dwm.present_confirmed,
            (unsigned long long)dwm.refresh_confirmed);
    if (dwm.available && frequency.QuadPart != 0) {
        const double refresh_ms =
            (double)dwm.refresh_period / 10000.0;
        fprintf(file, "DWM refresh period: %.4fms (%.3fHz)\n", refresh_ms,
                refresh_ms > 0.0 ? 1000.0 / refresh_ms : 0.0);
    }

    const unsigned int count =
        record_count < present_diag_max_records ? record_count : present_diag_max_records;
    const unsigned int start =
        record_count < present_diag_max_records ? 0 : record_start;

    uint64_t intervals[present_diag_max_records] = {};
    unsigned int interval_count = 0;
    double duration_sum = 0.0;
    unsigned int duration_count = 0;
    unsigned int before_target = 0;
    unsigned int near_target = 0;
    unsigned int near_refresh = 0;
    unsigned int near_two_refresh = 0;
    unsigned int long_gaps = 0;

    const double target = Limiter::PresentRate();
    const uint64_t target_interval =
        target > 0.0 ? (uint64_t)((double)frequency.QuadPart / target) : 0;
    const unsigned int refresh =
        Limiter::DisplayRefresh() != 0 ? Limiter::DisplayRefresh()
                                       : (mode_ok ? mode.dmDisplayFrequency
                                                  : (unsigned int)device_caps_refresh);
    const uint64_t refresh_interval =
        refresh != 0 ? frequency.QuadPart / refresh : 0;

    for (unsigned int i = 0; i < count; ++i) {
        const PresentRecord& record = records[(start + i) % present_diag_max_records];
        if (record.interval != 0) {
            intervals[interval_count++] = record.interval;
            if (target_interval != 0) {
                if (record.interval * 10 < target_interval * 5)
                    ++before_target;
                if (record.interval * 10 >= target_interval * 8 &&
                    record.interval * 10 <= target_interval * 12)
                    ++near_target;
                if (record.interval * 10 > target_interval * 15)
                    ++long_gaps;
            }
            if (refresh_interval != 0) {
                if (record.interval * 10 >= refresh_interval * 9 &&
                    record.interval * 10 <= refresh_interval * 11)
                    ++near_refresh;
                if (record.interval * 10 >= refresh_interval * 18 &&
                    record.interval * 10 <= refresh_interval * 22)
                    ++near_two_refresh;
            }
        }
        if (record.duration_known) {
            duration_sum += record.duration_ms;
            ++duration_count;
        }
    }

    fprintf(file,
            "last %u presents: target interval %.4fms, refresh interval %.4fms\n",
            count, qpc_ticks_to_ms(target_interval, frequency.QuadPart),
            qpc_ticks_to_ms(refresh_interval, frequency.QuadPart));
    if (interval_count != 0) {
        uint64_t interval_sum = 0;
        uint64_t interval_max = 0;
        for (unsigned int i = 0; i < interval_count; ++i) {
            interval_sum += intervals[i];
            if (intervals[i] > interval_max)
                interval_max = intervals[i];
        }
        for (unsigned int i = 1; i < interval_count; ++i) {
            const uint64_t value = intervals[i];
            unsigned int j = i;
            while (j > 0 && intervals[j - 1] > value) {
                intervals[j] = intervals[j - 1];
                --j;
            }
            intervals[j] = value;
        }
        const uint64_t p50 = intervals[interval_count / 2];
        const uint64_t p95 = intervals[(interval_count * 95) / 100];
        fprintf(file,
                "interval average %.4fms, p50 %.4fms, p95 %.4fms, max %.4fms, "
                "effective %.3f presents/s\n",
                qpc_ticks_to_ms(interval_sum, frequency.QuadPart) / interval_count,
                qpc_ticks_to_ms(p50, frequency.QuadPart),
                qpc_ticks_to_ms(p95, frequency.QuadPart),
                qpc_ticks_to_ms(interval_max, frequency.QuadPart),
                interval_sum != 0
                    ? (double)interval_count * (double)frequency.QuadPart /
                          (double)interval_sum
                    : 0.0);
        fprintf(file,
                "interval buckets: <half target %u, near target %u, near refresh %u, "
                "near 2x refresh %u, >1.5 target %u\n",
                before_target, near_target, near_refresh, near_two_refresh, long_gaps);
    }
    if (duration_count != 0) {
        fprintf(file, "Present blocking duration: average %.4fms over %u measured call(s)\n",
                duration_sum / duration_count, duration_count);
    } else {
        fprintf(file, "Present blocking duration: not observable on this D3D path\n");
    }

    fprintf(file, "recorded presents %llu, first QPC %llu, last QPC %llu\n",
            (unsigned long long)total_presents, (unsigned long long)first_qpc,
            (unsigned long long)previous_qpc);
    fprintf(file, "last %u presents (QPC ms, interval ms, Present ms or -1)\n", count);
    for (unsigned int i = 0; i < count; ++i) {
        const PresentRecord& record = records[(start + i) % present_diag_max_records];
        const double since =
            first_qpc != 0 && frequency.QuadPart != 0
                ? qpc_ticks_to_ms(record.qpc - first_qpc, frequency.QuadPart)
                : 0.0;
        fprintf(file, "%4u  %9.4f  %9.4f  %.4f\n", i, since,
                qpc_ticks_to_ms(record.interval, frequency.QuadPart),
                record.duration_known ? record.duration_ms : -1.0);
    }

    fclose(file);
    printf("Wrote oilp_display_diag.txt\n");
    return true;
}
}

void present_diag_set_window(void* window) {
    diagnostic_window = (HWND)window;
}

void present_diag_note_present_duration(uint64_t start_qpc, uint64_t end_qpc) {
    pending_duration_start = start_qpc;
    pending_duration_end = end_qpc;
}

void present_diag_mark_presentation(uint64_t qpc) {
    static uint64_t frequency;
    if (frequency == 0) {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        frequency = (uint64_t)value.QuadPart;
    }

    if (first_qpc == 0)
        first_qpc = qpc;

    PresentRecord& record = records[record_start];
    record = {};
    record.qpc = qpc;
    record.interval = previous_qpc != 0 ? qpc - previous_qpc : 0;
    if (pending_duration_end >= pending_duration_start && pending_duration_start != 0) {
        record.duration_known = true;
        pending_duration_end =
            pending_duration_end - pending_duration_start;
        pending_duration_start = 0;
    }
    if (record.duration_known) {
        record.duration_ms =
            (double)qpc_ticks_to_ms(pending_duration_end, frequency);
        pending_duration_end = 0;
    }
    previous_qpc = qpc;
    ++total_presents;
    record_start = (record_start + 1) % present_diag_max_records;
    if (record_count < present_diag_max_records)
        ++record_count;

    const bool down = (GetAsyncKeyState('U') & 0x8000) != 0;
    if (down && !hotkey_down)
        write_display_diag();
    hotkey_down = down;
}
