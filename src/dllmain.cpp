#include <windows.h>
#include <cstdint>
#include <cmath>
#include <atomic>

#include "logger.h"
#include "config.h"
#include "hook_util.h"

namespace {

// System dinput8.dll module and forwarded exports
HMODULE g_realDInput8 = nullptr;

using DirectInput8Create_t  = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using DllCanUnloadNow_t     = HRESULT(WINAPI*)();
using DllGetClassObject_t   = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
using DllRegisterServer_t   = HRESULT(WINAPI*)();
using DllUnregisterServer_t = HRESULT(WINAPI*)();
using GetdfDIJoystick_t     = const void*(WINAPI*)();

DirectInput8Create_t  g_origDirectInput8Create  = nullptr;
DllCanUnloadNow_t     g_origDllCanUnloadNow     = nullptr;
DllGetClassObject_t   g_origDllGetClassObject   = nullptr;
DllRegisterServer_t   g_origDllRegisterServer   = nullptr;
DllUnregisterServer_t g_origDllUnregisterServer = nullptr;
GetdfDIJoystick_t     g_origGetdfDIJoystick     = nullptr;

INIT_ONCE g_initOnce = INIT_ONCE_STATIC_INIT;

fsr::Config g_config;
fsr::TrampolineArena g_arena;
uintptr_t g_imageBase = 0;

// Game RVAs for Fate/Samurai Remnant (Steam Build 15597839, PE TimeDateStamp 0x66D51D08)
constexpr uint32_t EXPECTED_PE_TIMESTAMP      = 0x66D51D08;
constexpr uintptr_t RVA_POLL_MOUSE            = 0x0141EDB0; // FUN_14141edb0
constexpr uintptr_t RVA_CLAMP_LX              = 0x0141EEAA; // mov [rsi], eax (2 bytes)
constexpr uintptr_t RVA_CLAMP_LY              = 0x0141EED8; // mov [rbx+0x68], edx (3 bytes)
constexpr uintptr_t RVA_MOUSE_ELEMENT_UPDATE  = 0x02356D90; // FUN_142356d90
constexpr uintptr_t RVA_READ_PAD_AXIS         = 0x00062DF0; // FUN_140062df0
constexpr uintptr_t RVA_CAM_MOUSE_X           = 0x01E14469; // FUN_141e142c0 Mouse X deadzone block
constexpr uintptr_t RVA_CAM_MOUSE_Y           = 0x01E145AE; // FUN_141e142c0 Mouse Y deadzone block

// Diagnostic globals in game image (read-only inspection)
constexpr uintptr_t RVA_PARAM_TABLE_ROOT      = 0x0397D710; // DAT_14397d710
constexpr uintptr_t RVA_CAM_MODE_INDEX        = 0x03841B1C; // DAT_143841b1c (0 = Field, 1 = Battle)
constexpr uintptr_t RVA_FRAME_SUBSTEPS        = 0x03842154; // DAT_143842154 (60Hz sub-steps)

// Original function pointers populated by InstallDetour5
using FnPollMouse_t          = uint8_t(__fastcall*)(uint64_t mouseDev);
using FnMouseElementUpdate_t = void(__fastcall*)(uint64_t elem);
using FnReadPadAxis_t        = float(__fastcall*)(uint64_t playerIdx, int32_t axisIdx);

FnPollMouse_t          Original_FUN_14141edb0 = nullptr;
FnMouseElementUpdate_t Original_FUN_142356d90 = nullptr;
FnReadPadAxis_t        Original_FUN_140062df0 = nullptr;

// Per-tick mouse state shared between the input-poll phase (FUN_142357780) and
// the camera-update phase (FUN_141e142c0 -> FUN_140062df0).
bool    g_polledEarlyThisTick = false;
uint8_t g_lastPollResult      = 0;
bool    g_mouseActiveThisTick = false;
float   g_tickDeltaX          = 0.0f;
float   g_tickDeltaY          = 0.0f;

// Diagnostic telemetry counters
bool     g_loggedRuntimeParams = false;
uint32_t g_loggedMoveSamples   = 0;
uint64_t g_totalActiveTicks    = 0;

// Hook for FUN_142356d90 (Mouse virtual-pad element update, called from FUN_142357780)
void __fastcall Hook_FUN_142356d90(uint64_t elem) {
    uint64_t mouseDev = (elem != 0) ? *reinterpret_cast<uint64_t*>(elem + 0x08) : 0;

    // Fix the 1-tick polling order bug: poll DirectInput GetDeviceState BEFORE FUN_142356d90
    // reads mouseDev + 0x64 (lX) and mouseDev + 0x68 (lY).
    if (g_config.fixPollingOrder && mouseDev != 0 && Original_FUN_14141edb0 != nullptr) {
        g_lastPollResult = Original_FUN_14141edb0(mouseDev);
        g_polledEarlyThisTick = true;
    }

    // Write a sentinel into axis[4] before calling Original_FUN_142356d90 so we know with 100%
    // accuracy whether Original_FUN_142356d90 passed its internal window/client-area checks.
    constexpr float kSentinel = -9999.0f;
    float* axisBuf = (elem != 0) ? *reinterpret_cast<float**>(elem + 0x68) : nullptr;
    uint64_t axisCount = (elem != 0) ? *reinterpret_cast<uint64_t*>(elem + 0x70) : 0;
    bool canUseSentinel = (axisBuf != nullptr && axisCount > 5);

    if (canUseSentinel) {
        axisBuf[4] = kSentinel;
    }

    Original_FUN_142356d90(elem);

    if (canUseSentinel && axisBuf[4] == kSentinel) {
        // Original_FUN_142356d90 returned early (cursor outside client area or on title bar).
        axisBuf[4] = 0.0f;
        g_mouseActiveThisTick = false;
        g_tickDeltaX = 0.0f;
        g_tickDeltaY = 0.0f;
        return;
    }

    if (mouseDev != 0 && canUseSentinel) {
        int32_t lX = *reinterpret_cast<int32_t*>(mouseDev + 0x64);
        int32_t lY = *reinterpret_cast<int32_t*>(mouseDev + 0x68);
        g_tickDeltaX = static_cast<float>(lX);
        g_tickDeltaY = static_cast<float>(lY);
        g_mouseActiveThisTick = true;

        // Log runtime D and F (fVar20) on the first non-saturated mouse movement
        if (fsr::Logger::IsEnabled() && (lX != 0 || lY != 0) && g_loggedMoveSamples < 25) {
            float divisorD = *reinterpret_cast<float*>(elem + 0xA0);
            float stage2X = axisBuf[4];
            float stage2Y = axisBuf[5];
            float inferredFX = (lX != 0 && std::abs(lX) < 100 && divisorD > 0.0f)
                ? (std::fabs(stage2X) - static_cast<float>(std::abs(lX)) / divisorD)
                : 0.0f;
            ++g_loggedMoveSamples;
            fsr::Logger::Log(
                "[Sample #%u] Raw lX=%d lY=%d | D=%.1f | stage2 axis[4]=%.5f axis[5]=%.5f (inferred F=%.5f)",
                g_loggedMoveSamples, lX, lY, divisorD, stage2X, stage2Y, inferredFX);
        }
    } else {
        g_mouseActiveThisTick = false;
        g_tickDeltaX = 0.0f;
        g_tickDeltaY = 0.0f;
    }
}

// Hook for FUN_14141edb0 (DirectInput mouse Poll + GetDeviceState)
uint8_t __fastcall Hook_FUN_14141edb0(uint64_t mouseDev) {
    if (g_polledEarlyThisTick) {
        g_polledEarlyThisTick = false;
        return g_lastPollResult;
    }

    // If FUN_142356d90 was NOT called prior to FUN_14141edb0 on this tick, the mouse pad element
    // is disabled or the window is unfocused. Clear tick deltas so no stale delta can be read.
    if (g_config.fixPollingOrder) {
        g_mouseActiveThisTick = false;
        g_tickDeltaX = 0.0f;
        g_tickDeltaY = 0.0f;
    }

    return Original_FUN_14141edb0(mouseDev);
}

// Hook for FUN_140062df0 (Read pad axis value)
float __fastcall Hook_FUN_140062df0(uint64_t playerIdx, int32_t axisIdx) {
    float orig = Original_FUN_140062df0(playerIdx, axisIdx);

    if (axisIdx == 4 || axisIdx == 5) {
        if (!g_loggedRuntimeParams && fsr::Logger::IsEnabled() && g_imageBase != 0) {
            g_loggedRuntimeParams = true;
            uint64_t paramRoot = *reinterpret_cast<uint64_t*>(g_imageBase + RVA_PARAM_TABLE_ROOT);
            int32_t camMode = *reinterpret_cast<int32_t*>(g_imageBase + RVA_CAM_MODE_INDEX);
            int32_t subSteps = *reinterpret_cast<int32_t*>(g_imageBase + RVA_FRAME_SUBSTEPS);
            if (paramRoot != 0) {
                float fieldPitch  = *reinterpret_cast<float*>(paramRoot + 0x48CB78);
                float fieldYaw    = *reinterpret_cast<float*>(paramRoot + 0x48CB7C);
                float battlePitch = *reinterpret_cast<float*>(paramRoot + 8 + 0x48CB78);
                float battleYaw   = *reinterpret_cast<float*>(paramRoot + 8 + 0x48CB7C);
                fsr::Logger::Log(
                    "[CameraParams] camMode=%d subSteps=%d | Field(pitch=%.5f, yaw=%.5f) | Battle(pitch=%.5f, yaw=%.5f)",
                    camMode, subSteps, fieldPitch, fieldYaw, battlePitch, battleYaw);
            }
        }

        // If the original function returned 0.0f, either the mouse did not move on this axis or
        // one of the engine's focus / client-area / pad-enable gates blocked mouse input.
        if (!g_mouseActiveThisTick || orig == 0.0f) {
            return 0.0f;
        }

        float rawCounts = (axisIdx == 4) ? g_tickDeltaX : g_tickDeltaY;

        // Match the game's differential sensitivity slope:
        // Original above-threshold slope is d/d(lX) [ ((lX / 255.0) * 127.0 - 68.0) / 59.0 ]
        // = 127.0 / (255.0 * 59.0) = 127.0 / 15045.0 (~0.0084413426f per count).
        constexpr float kBaseSlope = 127.0f / (255.0f * 59.0f);
        return rawCounts * kBaseSlope * g_config.sensitivityScale;
    }

    return orig;
}

void LoadRealDInput8() {
    if (g_realDInput8 != nullptr) {
        return;
    }

    wchar_t sysDir[MAX_PATH] = {};
    UINT len = GetSystemDirectoryW(sysDir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return;
    }

    wchar_t dllPath[MAX_PATH] = {};
    _snwprintf_s(dllPath, MAX_PATH, _TRUNCATE, L"%s\\dinput8.dll", sysDir);
    g_realDInput8 = LoadLibraryW(dllPath);
    if (!g_realDInput8) {
        return;
    }

    g_origDirectInput8Create  = reinterpret_cast<DirectInput8Create_t>(GetProcAddress(g_realDInput8, "DirectInput8Create"));
    g_origDllCanUnloadNow     = reinterpret_cast<DllCanUnloadNow_t>(GetProcAddress(g_realDInput8, "DllCanUnloadNow"));
    g_origDllGetClassObject   = reinterpret_cast<DllGetClassObject_t>(GetProcAddress(g_realDInput8, "DllGetClassObject"));
    g_origDllRegisterServer   = reinterpret_cast<DllRegisterServer_t>(GetProcAddress(g_realDInput8, "DllRegisterServer"));
    g_origDllUnregisterServer = reinterpret_cast<DllUnregisterServer_t>(GetProcAddress(g_realDInput8, "DllUnregisterServer"));
    g_origGetdfDIJoystick     = reinterpret_cast<GetdfDIJoystick_t>(GetProcAddress(g_realDInput8, "GetdfDIJoystick"));
}

BOOL CALLBACK InitModOnceCallback(PINIT_ONCE, PVOID, PVOID*) {
    g_config.Load();
    fsr::Logger::Init(g_config.enableLogging);

    fsr::Logger::Log("Config loaded (iniFound=%d): EnableFix=%d SensitivityScale=%.4f FixPollingOrder=%d UnclampDirectInput=%d ScaleByFrameStep=%d",
                     g_config.iniFound ? 1 : 0,
                     g_config.enableFix ? 1 : 0,
                     g_config.sensitivityScale,
                     g_config.fixPollingOrder ? 1 : 0,
                     g_config.unclampDirectInput ? 1 : 0,
                     g_config.scaleByFrameStep ? 1 : 0);

    HMODULE exeMod = GetModuleHandleW(nullptr);
    g_imageBase = reinterpret_cast<uintptr_t>(exeMod);
    fsr::Logger::Log("Executable base address: 0x%llX", static_cast<unsigned long long>(g_imageBase));

    // Inspect PE TimeDateStamp
    const auto* dosHdr = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_imageBase);
    if (dosHdr->e_magic == IMAGE_DOS_SIGNATURE) {
        const auto* ntHdr = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_imageBase + dosHdr->e_lfanew);
        if (ntHdr->Signature == IMAGE_NT_SIGNATURE) {
            uint32_t ts = ntHdr->FileHeader.TimeDateStamp;
            fsr::Logger::Log("Executable PE TimeDateStamp: 0x%08X (expected 0x%08X)", ts, EXPECTED_PE_TIMESTAMP);
        }
    }

    if (!g_config.enableFix) {
        fsr::Logger::Log("EnableFix=0 in fsr_mouse_fix.ini; leaving executable unpatched.");
        return TRUE;
    }

    // Expected original bytes at all 7 target locations
    static const uint8_t kExpPollPrologue[5] = { 0x48, 0x89, 0x5C, 0x24, 0x18 };
    static const uint8_t kExpClampLX[2]      = { 0x89, 0x06 };
    static const uint8_t kExpClampLY[3]      = { 0x89, 0x53, 0x68 };
    static const uint8_t kExpElemPrologue[5] = { 0x4C, 0x8B, 0xDC, 0x56, 0x57 };
    static const uint8_t kExpAxisPrologue[5] = { 0x48, 0x89, 0x5C, 0x24, 0x08 };
    static const uint8_t kExpCamMouseX[21]   = {
        0xF3, 0x41, 0x0F, 0x59, 0xC6,
        0xF3, 0x0F, 0x2C, 0xC8,
        0x8B, 0xC1,
        0x99,
        0x33, 0xC2,
        0x2B, 0xC2,
        0x83, 0xF8, 0x44,
        0x7C, 0x66
    };
    static const uint8_t kExpCamMouseY[21]   = {
        0xF3, 0x41, 0x0F, 0x59, 0xC6,
        0xF3, 0x0F, 0x2C, 0xC8,
        0x8B, 0xC1,
        0x99,
        0x33, 0xC2,
        0x2B, 0xC2,
        0x83, 0xF8, 0x44,
        0x7C, 0x69
    };

    // Pre-verify all sites before modifying a single byte
    bool allValid = true;
    allValid &= fsr::VerifyBytes(g_imageBase + RVA_POLL_MOUSE, kExpPollPrologue, sizeof(kExpPollPrologue), "FUN_14141edb0 prologue");
    if (g_config.unclampDirectInput) {
        allValid &= fsr::VerifyBytes(g_imageBase + RVA_CLAMP_LX, kExpClampLX, sizeof(kExpClampLX), "FUN_14141edb0 lX clamp");
        allValid &= fsr::VerifyBytes(g_imageBase + RVA_CLAMP_LY, kExpClampLY, sizeof(kExpClampLY), "FUN_14141edb0 lY clamp");
    }
    allValid &= fsr::VerifyBytes(g_imageBase + RVA_MOUSE_ELEMENT_UPDATE, kExpElemPrologue, sizeof(kExpElemPrologue), "FUN_142356d90 prologue");
    allValid &= fsr::VerifyBytes(g_imageBase + RVA_READ_PAD_AXIS, kExpAxisPrologue, sizeof(kExpAxisPrologue), "FUN_140062df0 prologue");
    allValid &= fsr::VerifyBytes(g_imageBase + RVA_CAM_MOUSE_X, kExpCamMouseX, sizeof(kExpCamMouseX), "FUN_141e142c0 Mouse X block");
    allValid &= fsr::VerifyBytes(g_imageBase + RVA_CAM_MOUSE_Y, kExpCamMouseY, sizeof(kExpCamMouseY), "FUN_141e142c0 Mouse Y block");

    if (!allValid) {
        fsr::Logger::Log("[ABORT] One or more byte verifications failed. Game binary left completely untouched.");
        return TRUE;
    }

    if (!g_arena.InitNear(g_imageBase)) {
        fsr::Logger::Log("[ABORT] Could not allocate trampoline arena. Game binary left completely untouched.");
        return TRUE;
    }

    // 1. Optional DirectInput lX/lY virtual-cursor writeback unclamp
    if (g_config.unclampDirectInput) {
        static const uint8_t kNop2[2] = { 0x90, 0x90 };
        static const uint8_t kNop3[3] = { 0x90, 0x90, 0x90 };
        fsr::PatchBytes(g_imageBase + RVA_CLAMP_LX, kNop2, sizeof(kNop2), "Unclamp DirectInput lX");
        fsr::PatchBytes(g_imageBase + RVA_CLAMP_LY, kNop3, sizeof(kNop3), "Unclamp DirectInput lY");
    }

    // 2. Patch FUN_141e142c0 Mouse X and Mouse Y deadzone/quantization blocks
    // Mouse X (19 bytes at RVA 0x1e14469, jumping to 0x1e144e4):
    //   0F 28 F0             movaps xmm6, xmm0
    //   F3 41 0F 59 F4       mulss  xmm6, xmm12   (-1.0f)
    //   F3 0F 59 F7          mulss  xmm6, xmm7    (yaw_speed)
    //   [optional mulss xmm6, xmm15 or 5-byte NOP]
    //   EB 68                jmp    +0x68 (0x141e144e4)
    uint8_t patchMouseX[19] = {
        0x0F, 0x28, 0xF0,
        0xF3, 0x41, 0x0F, 0x59, 0xF4,
        0xF3, 0x0F, 0x59, 0xF7,
        0x0F, 0x1F, 0x44, 0x00, 0x00,
        0xEB, 0x68
    };
    if (g_config.scaleByFrameStep) {
        const uint8_t mulXmm6Xmm15[5] = { 0xF3, 0x41, 0x0F, 0x59, 0xF7 };
        std::memcpy(patchMouseX + 12, mulXmm6Xmm15, 5);
    }

    // Mouse Y (21 bytes at RVA 0x1e145ae, jumping to 0x1e1462c):
    //   44 0F 28 C0          movaps xmm8, xmm0
    //   F3 45 0F 59 C4       mulss  xmm8, xmm12   (-1.0f)
    //   F3 44 0F 59 C7       mulss  xmm8, xmm7    (pitch_speed)
    //   [optional mulss xmm8, xmm15 or 5-byte NOP]
    //   EB 69                jmp    +0x69 (0x141e1462c)
    uint8_t patchMouseY[21] = {
        0x44, 0x0F, 0x28, 0xC0,
        0xF3, 0x45, 0x0F, 0x59, 0xC4,
        0xF3, 0x44, 0x0F, 0x59, 0xC7,
        0x0F, 0x1F, 0x44, 0x00, 0x00,
        0xEB, 0x69
    };
    if (g_config.scaleByFrameStep) {
        const uint8_t mulXmm8Xmm15[5] = { 0xF3, 0x45, 0x0F, 0x59, 0xC7 };
        std::memcpy(patchMouseY + 14, mulXmm8Xmm15, 5);
    }

    fsr::PatchBytes(g_imageBase + RVA_CAM_MOUSE_X, patchMouseX, sizeof(patchMouseX), "FUN_141e142c0 Mouse X continuous bypass");
    fsr::PatchBytes(g_imageBase + RVA_CAM_MOUSE_Y, patchMouseY, sizeof(patchMouseY), "FUN_141e142c0 Mouse Y continuous bypass");

    // 3. Install function detours
    g_arena.InstallDetour5(
        g_imageBase + RVA_POLL_MOUSE,
        reinterpret_cast<void*>(&Hook_FUN_14141edb0),
        reinterpret_cast<void**>(&Original_FUN_14141edb0),
        kExpPollPrologue,
        "FUN_14141edb0 (PollMouse)");

    g_arena.InstallDetour5(
        g_imageBase + RVA_MOUSE_ELEMENT_UPDATE,
        reinterpret_cast<void*>(&Hook_FUN_142356d90),
        reinterpret_cast<void**>(&Original_FUN_142356d90),
        kExpElemPrologue,
        "FUN_142356d90 (MouseElementUpdate)");

    g_arena.InstallDetour5(
        g_imageBase + RVA_READ_PAD_AXIS,
        reinterpret_cast<void*>(&Hook_FUN_140062df0),
        reinterpret_cast<void**>(&Original_FUN_140062df0),
        kExpAxisPrologue,
        "FUN_140062df0 (ReadPadAxis)");

    fsr::Logger::Log("[SUCCESS] All Fate/Samurai Remnant mouse camera fixes installed active and verified.");
    return TRUE;
}

DWORD WINAPI InitWaitThreadProc(LPVOID) {
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (base == 0) {
        return 0;
    }

    static const uint8_t kExpPollPrologue[5] = { 0x48, 0x89, 0x5C, 0x24, 0x18 };
    const uint8_t* pollPtr = reinterpret_cast<const uint8_t*>(base + RVA_POLL_MOUSE);

    // Wait up to 30 seconds for SteamStub to finish unpacking .text in memory.
    // When loaded via Ultimate ASI Loader as FSRMouseFix.asi, this thread automatically
    // installs the hooks as soon as .text is decrypted, without needing DirectInput8Create
    // to pass through our DLL.
    for (int i = 0; i < 3000; ++i) {
        if (std::memcmp(pollPtr, kExpPollPrologue, sizeof(kExpPollPrologue)) == 0 || pollPtr[0] == 0xE9) {
            InitOnceExecuteOnce(&g_initOnce, InitModOnceCallback, nullptr, nullptr);
            return 0;
        }
        Sleep(10);
    }

    // Timeout fallback: run InitModOnceCallback once so any byte mismatch is logged.
    InitOnceExecuteOnce(&g_initOnce, InitModOnceCallback, nullptr, nullptr);
    return 0;
}

} // namespace

extern "C" {

HRESULT WINAPI Proxy_DirectInput8Create(
    HINSTANCE hinst,
    DWORD dwVersion,
    REFIID riidltf,
    LPVOID* ppvOut,
    LPUNKNOWN punkOuter) {

    LoadRealDInput8();
    InitOnceExecuteOnce(&g_initOnce, InitModOnceCallback, nullptr, nullptr);

    if (!g_origDirectInput8Create) {
        return E_FAIL;
    }
    return g_origDirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter);
}

HRESULT WINAPI Proxy_DllCanUnloadNow() {
    LoadRealDInput8();
    return g_origDllCanUnloadNow ? g_origDllCanUnloadNow() : S_FALSE;
}

HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    LoadRealDInput8();
    return g_origDllGetClassObject ? g_origDllGetClassObject(rclsid, riid, ppv) : CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI Proxy_DllRegisterServer() {
    LoadRealDInput8();
    return g_origDllRegisterServer ? g_origDllRegisterServer() : E_FAIL;
}

HRESULT WINAPI Proxy_DllUnregisterServer() {
    LoadRealDInput8();
    return g_origDllUnregisterServer ? g_origDllUnregisterServer() : E_FAIL;
}

const void* WINAPI Proxy_GetdfDIJoystick() {
    LoadRealDInput8();
    return g_origGetdfDIJoystick ? g_origGetdfDIJoystick() : nullptr;
}

} // extern "C"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        HANDLE hThread = CreateThread(nullptr, 0, InitWaitThreadProc, nullptr, 0, nullptr);
        if (hThread != nullptr) {
            CloseHandle(hThread);
        }
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        fsr::Logger::Shutdown();
    }
    return TRUE;
}
