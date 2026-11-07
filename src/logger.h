#pragma once

#include <windows.h>
#include <cstdio>
#include <cstdarg>

namespace fsr {

class Logger {
public:
    static void Init(bool enabled) {
        InitializeCriticalSection(&s_cs);
        s_csInit = true;
        s_enabled = enabled;
        if (!s_enabled) {
            return;
        }

        wchar_t tempDir[MAX_PATH] = {};
        DWORD len = GetTempPathW(MAX_PATH, tempDir);
        if (len == 0 || len >= MAX_PATH) {
            return;
        }

        wchar_t logPath[MAX_PATH] = {};
        _snwprintf_s(logPath, MAX_PATH, _TRUNCATE, L"%sfsr_mouse_fix.log", tempDir);

        s_file = CreateFileW(
            logPath,
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (s_file != INVALID_HANDLE_VALUE) {
            Log("================================================================");
            Log("Fate/Samurai Remnant - Raw Mouse Input Fix Log");
            Log("================================================================");
        }
    }

    static void Shutdown() {
        if (!s_csInit) {
            return;
        }
        EnterCriticalSection(&s_cs);
        if (s_file != INVALID_HANDLE_VALUE) {
            CloseHandle(s_file);
            s_file = INVALID_HANDLE_VALUE;
        }
        LeaveCriticalSection(&s_cs);
    }

    static void Log(const char* fmt, ...) {
        if (!s_enabled || !s_csInit || s_file == INVALID_HANDLE_VALUE) {
            return;
        }

        SYSTEMTIME st = {};
        GetLocalTime(&st);

        char body[1024] = {};
        va_list args;
        va_start(args, fmt);
        _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, args);
        va_end(args);

        char line[1152] = {};
        int lineLen = _snprintf_s(
            line,
            sizeof(line),
            _TRUNCATE,
            "[%02u:%02u:%02u.%03u] %s\r\n",
            st.wHour,
            st.wMinute,
            st.wSecond,
            st.wMilliseconds,
            body);

        if (lineLen > 0) {
            EnterCriticalSection(&s_cs);
            if (s_file != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(s_file, line, static_cast<DWORD>(lineLen), &written, nullptr);
            }
            LeaveCriticalSection(&s_cs);
        }
    }

    static bool IsEnabled() { return s_enabled; }

private:
    static inline CRITICAL_SECTION s_cs = {};
    static inline bool s_csInit = false;
    static inline bool s_enabled = false;
    static inline HANDLE s_file = INVALID_HANDLE_VALUE;
};

} // namespace fsr
