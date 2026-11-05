#include "AsrOpener.h"

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

// --- Signatures (EuroScope 3.2.x, x86) ---------------------------------------
//
// Inside the command-line handler that serves the F1+<digit> ASR fast keys:
//
//   push   offset "ASRFastKeys"
//   call   GetProfileValue
//   ...
//   call   GetApplication                 E8 ?? ?? ?? ??
//   lea    ecx,[ebp-xx]                   8D 4D ??
//   push   ecx                            51
//   mov    ecx,eax                        8B C8
//   call   GetAsrManager                  E8 ?? ?? ?? ??   (returns shared_ptr)
//   mov    byte ptr [ebp-4],xx            C6 45 FC ??
//   push   dword ptr [ebp-xx]   ; path    FF 75 ??
//   mov    ecx,dword ptr [eax]  ; manager 8B 08
//   call   OpenOrActivateAsr              E8 ?? ?? ?? ??
//
// OpenOrActivateAsr(const char* path) walks the list of open ASRs; if one with
// the same path is open it is activated, otherwise the file is opened.

const int kWild = -1;
const int kCallPattern[] = {0xE8, kWild, kWild, kWild, kWild, 0x8D, 0x4D, kWild, 0x51, 0x8B, 0xC8,
                            0xE8, kWild, kWild, kWild, kWild, 0xC6, 0x45, 0xFC, kWild, 0xFF, 0x75,
                            kWild, 0x8B, 0x08, 0xE8, kWild, kWild, kWild, kWild};
const size_t kCallPatternLen = sizeof(kCallPattern) / sizeof(kCallPattern[0]);

// push ebp / mov ebp,esp / push ebx / mov ebx,[ebp+8] / push esi / push edi / mov edi,ecx
const uint8_t kOpenPrologue[] = {0x55, 0x8B, 0xEC, 0x53, 0x8B, 0x5D, 0x08, 0x56, 0x57, 0x8B, 0xF9};
// push ebp / mov ebp,esp
const uint8_t kFramePrologue[] = {0x55, 0x8B, 0xEC};

const size_t kMaxDistanceFromString = 0x100;

struct SharedPtrRaw {
    void* object;
    void* control; // MSVC _Ref_count_base: { vtable, uses, weaks }
};

using GetApplicationFn = void*(__cdecl*)();
// thiscall emulated with fastcall: ecx = this, edx = unused; callee pops stack args.
using GetAsrManagerFn = SharedPtrRaw*(__fastcall*)(void* self, void* unused, SharedPtrRaw* out);
using OpenOrActivateFn = bool(__fastcall*)(void* self, void* unused, const char* path);
using RefCountVirtualFn = void(__fastcall*)(void* self, void* unused);

GetApplicationFn g_getApplication = nullptr;
GetAsrManagerFn g_getAsrManager = nullptr;
OpenOrActivateFn g_openOrActivate = nullptr;
bool g_initialised = false;

struct Section {
    uint8_t* begin = nullptr;
    uint8_t* end = nullptr;
};

bool Contains(const Section& s, const uint8_t* p, size_t len = 1)
{
    return p >= s.begin && p + len <= s.end;
}

bool MatchAt(const uint8_t* p, const int* pattern, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        if (pattern[i] != kWild && p[i] != static_cast<uint8_t>(pattern[i])) {
            return false;
        }
    }
    return true;
}

uint8_t* CallTarget(uint8_t* callInstruction)
{
    int32_t rel;
    std::memcpy(&rel, callInstruction + 1, sizeof(rel));
    return callInstruction + 5 + rel;
}

void CollectSections(HMODULE module, std::vector<Section>& code, std::vector<Section>& data)
{
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return;
    }
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        Section s;
        s.begin = base + sec->VirtualAddress;
        DWORD size = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
        s.end = s.begin + size;
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            code.push_back(s);
        } else if (sec->Characteristics & IMAGE_SCN_MEM_READ) {
            data.push_back(s);
        }
    }
}

// Calls the located routines. Only POD locals here so that SEH can be used.
int CallOpenGuarded(const char* path)
{
    __try {
        void* app = g_getApplication();
        if (!app) {
            return 0;
        }
        SharedPtrRaw holder = {nullptr, nullptr};
        SharedPtrRaw* sp = g_getAsrManager(app, nullptr, &holder);
        void* manager = sp ? sp->object : nullptr;
        bool ok = false;
        if (manager) {
            ok = g_openOrActivate(manager, nullptr, path);
        }
        // Release our copy of the shared_ptr exactly like std::shared_ptr would.
        if (holder.control) {
            auto* counts = reinterpret_cast<volatile long*>(holder.control);
            void** vtable = *reinterpret_cast<void***>(holder.control);
            if (InterlockedDecrement(&counts[1]) == 0) {
                reinterpret_cast<RefCountVirtualFn>(vtable[0])(holder.control, nullptr);
                if (InterlockedDecrement(&counts[2]) == 0) {
                    reinterpret_cast<RefCountVirtualFn>(vtable[1])(holder.control, nullptr);
                }
            }
        }
        return ok ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

} // namespace

namespace AsrOpener {

bool Initialise(std::string& diagnostic)
{
    if (g_initialised) {
        diagnostic = "already initialised";
        return true;
    }

    HMODULE exe = GetModuleHandleW(nullptr);
    std::vector<Section> code, data;
    CollectSections(exe, code, data);
    if (code.empty() || data.empty()) {
        diagnostic = "could not read EuroScope.exe sections";
        return false;
    }

    static const char kKey[] = "ASRFastKeys";
    const size_t keyLen = sizeof(kKey); // including terminating zero

    int candidates = 0;
    for (const Section& d : data) {
        for (uint8_t* s = d.begin; s + keyLen <= d.end; ++s) {
            if (*s != 'A' || std::memcmp(s, kKey, keyLen) != 0 || (s > d.begin && s[-1] != 0)) {
                continue;
            }
            uint8_t push[5] = {0x68};
            uint32_t address = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s));
            std::memcpy(push + 1, &address, sizeof(address));

            for (const Section& c : code) {
                for (uint8_t* p = c.begin; p + 5 <= c.end; ++p) {
                    if (p[0] != 0x68 || std::memcmp(p, push, 5) != 0) {
                        continue;
                    }
                    uint8_t* limit = p + kMaxDistanceFromString;
                    for (uint8_t* q = p + 5; q < limit && q + kCallPatternLen <= c.end; ++q) {
                        if (!MatchAt(q, kCallPattern, kCallPatternLen)) {
                            continue;
                        }
                        ++candidates;
                        uint8_t* getApp = CallTarget(q);
                        uint8_t* getMgr = CallTarget(q + 11);
                        uint8_t* open = CallTarget(q + 25);
                        if (!Contains(c, getApp, 8) || !Contains(c, getMgr, 8) ||
                            !Contains(c, open, sizeof(kOpenPrologue))) {
                            continue;
                        }
                        if (std::memcmp(getApp, kFramePrologue, sizeof(kFramePrologue)) != 0 ||
                            std::memcmp(getMgr, kFramePrologue, sizeof(kFramePrologue)) != 0 ||
                            std::memcmp(open, kOpenPrologue, sizeof(kOpenPrologue)) != 0) {
                            continue;
                        }
                        g_getApplication = reinterpret_cast<GetApplicationFn>(getApp);
                        g_getAsrManager = reinterpret_cast<GetAsrManagerFn>(getMgr);
                        g_openOrActivate = reinterpret_cast<OpenOrActivateFn>(open);
                        g_initialised = true;
                        char buf[160];
                        sprintf_s(buf, "ASR loader found at EuroScope.exe+0x%X",
                                  static_cast<unsigned>(open - reinterpret_cast<uint8_t*>(exe)));
                        diagnostic = buf;
                        return true;
                    }
                }
            }
        }
    }

    diagnostic = candidates ? "ASR loader signature did not validate for this EuroScope version"
                            : "ASR loader not found in this EuroScope version";
    return false;
}

bool Available()
{
    return g_initialised;
}

Result Open(const std::string& asrPath)
{
    if (!g_initialised) {
        return Result::NotAvailable;
    }
    int r = CallOpenGuarded(asrPath.c_str());
    if (r < 0) {
        // Never try again after a fault.
        g_initialised = false;
        return Result::Crashed;
    }
    return r ? Result::Opened : Result::Refused;
}

} // namespace AsrOpener
