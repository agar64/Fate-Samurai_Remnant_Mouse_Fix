#pragma once

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include "logger.h"

namespace fsr {

inline bool VerifyBytes(uintptr_t addr, const uint8_t* expected, size_t len, const char* label) {
    const uint8_t* actual = reinterpret_cast<const uint8_t*>(addr);
    if (std::memcmp(actual, expected, len) == 0) {
        Logger::Log("[OK] Verified bytes for %s at 0x%llX (%zu bytes)", label, static_cast<unsigned long long>(addr), len);
        return true;
    }

    char expHex[256] = {};
    char actHex[256] = {};
    size_t maxDump = len < 32 ? len : 32;
    for (size_t i = 0; i < maxDump; ++i) {
        _snprintf_s(expHex + i * 3, sizeof(expHex) - i * 3, _TRUNCATE, "%02X ", expected[i]);
        _snprintf_s(actHex + i * 3, sizeof(actHex) - i * 3, _TRUNCATE, "%02X ", actual[i]);
    }

    Logger::Log("[ERROR] Byte mismatch for %s at 0x%llX!", label, static_cast<unsigned long long>(addr));
    Logger::Log("  Expected: %s", expHex);
    Logger::Log("  Actual:   %s", actHex);
    return false;
}

inline bool PatchBytes(uintptr_t addr, const uint8_t* bytes, size_t len, const char* label) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), len, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Logger::Log("[ERROR] VirtualProtect failed for %s at 0x%llX (err=%lu)",
                    label, static_cast<unsigned long long>(addr), GetLastError());
        return false;
    }

    std::memcpy(reinterpret_cast<void*>(addr), bytes, len);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void*>(addr), len);

    DWORD dummy = 0;
    VirtualProtect(reinterpret_cast<void*>(addr), len, oldProtect, &dummy);

    Logger::Log("[OK] Patched %s at 0x%llX (%zu bytes)", label, static_cast<unsigned long long>(addr), len);
    return true;
}

class TrampolineArena {
public:
    bool InitNear(uintptr_t moduleBase) {
        if (m_page != nullptr) {
            return true;
        }

        SYSTEM_INFO si = {};
        GetSystemInfo(&si);
        uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;

        // Search downward and upward within +/- 1.5 GB of moduleBase so any RVA in the ~68 MB
        // image can reach the arena via a signed 32-bit E9 rel32 jump.
        const uintptr_t maxDelta = 0x58000000ULL;
        for (uintptr_t offset = gran; offset < maxDelta; offset += gran) {
            if (moduleBase > offset) {
                uintptr_t candidate = (moduleBase - offset) & ~(gran - 1);
                void* mem = VirtualAlloc(reinterpret_cast<void*>(candidate), 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                if (mem != nullptr) {
                    m_page = static_cast<uint8_t*>(mem);
                    m_used = 0;
                    Logger::Log("[OK] Allocated trampoline arena at 0x%llX (delta -0x%llX from base)",
                                reinterpret_cast<unsigned long long>(m_page),
                                static_cast<unsigned long long>(moduleBase - reinterpret_cast<uintptr_t>(m_page)));
                    return true;
                }
            }

            uintptr_t candidateUp = (moduleBase + offset) & ~(gran - 1);
            void* memUp = VirtualAlloc(reinterpret_cast<void*>(candidateUp), 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (memUp != nullptr) {
                m_page = static_cast<uint8_t*>(memUp);
                m_used = 0;
                Logger::Log("[OK] Allocated trampoline arena at 0x%llX (delta +0x%llX from base)",
                            reinterpret_cast<unsigned long long>(m_page),
                            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(m_page) - moduleBase));
                return true;
            }
        }

        Logger::Log("[ERROR] Failed to allocate trampoline arena within +/-1.5 GB of 0x%llX",
                    static_cast<unsigned long long>(moduleBase));
        return false;
    }

    // Installs a 5-byte E9 rel32 detour at targetAddr (whose first 5 bytes must contain no RIP-relative
    // displacements and must match expectedPrologue[5]).
    bool InstallDetour5(
        uintptr_t targetAddr,
        void* hookFn,
        void** outOrigFn,
        const uint8_t expectedPrologue[5],
        const char* name) {

        if (!m_page || m_used + 64 > 4096) {
            Logger::Log("[ERROR] Trampoline arena exhausted when installing %s", name);
            return false;
        }

        if (!VerifyBytes(targetAddr, expectedPrologue, 5, name)) {
            return false;
        }

        // 1. Build original-function trampoline: [5 stolen bytes] + [E9 rel32 to targetAddr + 5]
        uint8_t* tramp = m_page + m_used;
        std::memcpy(tramp, expectedPrologue, 5);

        int64_t backDelta = static_cast<int64_t>(targetAddr + 5) - static_cast<int64_t>(reinterpret_cast<uintptr_t>(tramp + 10));
        if (backDelta < INT32_MIN || backDelta > INT32_MAX) {
            Logger::Log("[ERROR] Trampoline back-jump out of rel32 range for %s", name);
            return false;
        }

        tramp[5] = 0xE9;
        int32_t backRel32 = static_cast<int32_t>(backDelta);
        std::memcpy(tramp + 6, &backRel32, 4);
        m_used += 16; // align next stub to 16 bytes

        // 2. Build 14-byte relay stub: FF 25 00 00 00 00 [64-bit hookFn]
        uint8_t* relay = m_page + m_used;
        relay[0] = 0xFF;
        relay[1] = 0x25;
        relay[2] = 0x00;
        relay[3] = 0x00;
        relay[4] = 0x00;
        relay[5] = 0x00;
        uint64_t hookAddr = reinterpret_cast<uint64_t>(hookFn);
        std::memcpy(relay + 6, &hookAddr, 8);
        m_used += 16;

        int64_t toRelayDelta = static_cast<int64_t>(reinterpret_cast<uintptr_t>(relay)) - static_cast<int64_t>(targetAddr + 5);
        if (toRelayDelta < INT32_MIN || toRelayDelta > INT32_MAX) {
            Logger::Log("[ERROR] Relay jump out of rel32 range for %s", name);
            return false;
        }

        FlushInstructionCache(GetCurrentProcess(), tramp, 32);
        *outOrigFn = reinterpret_cast<void*>(tramp);

        // 3. Patch 5-byte E9 rel32 at targetAddr
        uint8_t jmpPatch[5];
        jmpPatch[0] = 0xE9;
        int32_t toRelayRel32 = static_cast<int32_t>(toRelayDelta);
        std::memcpy(jmpPatch + 1, &toRelayRel32, 4);

        if (!PatchBytes(targetAddr, jmpPatch, 5, name)) {
            return false;
        }

        Logger::Log("[OK] Installed 5-byte detour for %s: target=0x%llX -> relay=0x%llX -> hook=0x%llX (tramp=0x%llX)",
                    name,
                    static_cast<unsigned long long>(targetAddr),
                    reinterpret_cast<unsigned long long>(relay),
                    static_cast<unsigned long long>(hookAddr),
                    reinterpret_cast<unsigned long long>(tramp));
        return true;
    }

private:
    uint8_t* m_page = nullptr;
    size_t m_used = 0;
};

} // namespace fsr
