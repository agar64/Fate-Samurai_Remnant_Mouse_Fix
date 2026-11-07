#pragma once

#include <windows.h>
#include <cstdlib>
#include <cwchar>

namespace fsr {

struct Config {
    bool enableFix = true;
    float sensitivityScale = 1.0f;
    bool fixPollingOrder = true;
    bool unclampDirectInput = true;
    bool scaleByFrameStep = false;
    bool enableLogging = true;

    wchar_t iniPath[MAX_PATH] = {};
    bool iniFound = false;

    void Load() {
        wchar_t exePath[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) {
            return;
        }

        wchar_t* lastSlash = wcsrchr(exePath, L'\\');
        if (!lastSlash) {
            lastSlash = wcsrchr(exePath, L'/');
        }
        if (lastSlash) {
            *(lastSlash + 1) = L'\0';
            _snwprintf_s(iniPath, MAX_PATH, _TRUNCATE, L"%sfsr_mouse_fix.ini", exePath);
        } else {
            wcscpy_s(iniPath, L"fsr_mouse_fix.ini");
        }

        DWORD attrs = GetFileAttributesW(iniPath);
        iniFound = (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY));

        enableFix = GetPrivateProfileIntW(L"MouseFix", L"EnableFix", 1, iniPath) != 0;
        fixPollingOrder = GetPrivateProfileIntW(L"MouseFix", L"FixPollingOrder", 1, iniPath) != 0;
        unclampDirectInput = GetPrivateProfileIntW(L"MouseFix", L"UnclampDirectInput", 1, iniPath) != 0;
        scaleByFrameStep = GetPrivateProfileIntW(L"MouseFix", L"ScaleByFrameStep", 0, iniPath) != 0;
        enableLogging = GetPrivateProfileIntW(L"MouseFix", L"EnableLogging", 1, iniPath) != 0;

        wchar_t scaleBuf[64] = {};
        GetPrivateProfileStringW(L"MouseFix", L"SensitivityScale", L"1.0", scaleBuf, 64, iniPath);
        wchar_t* endPtr = nullptr;
        float parsed = wcstof(scaleBuf, &endPtr);
        if (endPtr != scaleBuf && parsed > 0.001f && parsed <= 100.0f) {
            sensitivityScale = parsed;
        }
    }
};

} // namespace fsr
