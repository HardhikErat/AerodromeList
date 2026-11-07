#include "AsrOpener.h"

#include <windows.h>
#include <cctype>
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
// OpenOrActivateAsr(const char* path) walks the manager's list of open ASRs
// (manager+0x0C -> std::list; node+8 -> ASR object; ASR+8 -> CString path).
// If one has the same path it is activated, otherwise the file is opened.

const int kWild = -1;
const int kCallPattern[] = {0xE8, kWild, kWild, kWild, kWild, 0x8D, 0x4D, kWild, 0x51, 0x8B, 0xC8,
                            0xE8, kWild, kWild, kWild, kWild, 0xC6, 0x45, 0xFC, kWild, 0xFF, 0x75,
                            kWild, 0x8B, 0x08, 0xE8, kWild, kWild, kWild, kWild};
const size_t kCallPatternLen = sizeof(kCallPattern) / sizeof(kCallPattern[0]);

// push ebp / mov ebp,esp / push ebx / mov ebx,[ebp+8] / push esi / push edi / mov edi,ecx
// mov eax,[edi+0Ch] / mov esi,[eax] / cmp esi,eax / je xx / mov ecx,[esi+8] / add ecx,8
// The second line pins down the list layout used by OpenAsrPaths().
const int kOpenPrologue[] = {0x55, 0x8B, 0xEC, 0x53, 0x8B, 0x5D, 0x08, 0x56, 0x57, 0x8B, 0xF9,
                             0x8B, 0x47, 0x0C, 0x8B, 0x30, 0x3B, 0xF0, 0x74, kWild, 0x8B, 0x4E,
                             0x08, 0x83, 0xC1, 0x08};
const size_t kOpenPrologueLen = sizeof(kOpenPrologue) / sizeof(kOpenPrologue[0]);
const size_t kListOffset = 0x0C; // manager -> list head
const size_t kNodeValue = 0x08;  // list node -> ASR object
const size_t kAsrPath = 0x08;    // ASR object -> CString (char*)

// push ebp / mov ebp,esp
const uint8_t kFramePrologue[] = {0x55, 0x8B, 0xEC};

const size_t kMaxDistanceFromString = 0x100;
const int kMaxOpenAsrs = 64;
const int kMaxPath = 520;

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

// Scratch buffer for the open-ASR list (UI thread only).
struct OpenList {
    int count;
    char paths[kMaxOpenAsrs][kMaxPath];
};
OpenList g_openList;

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

void ReleaseShared(SharedPtrRaw& holder)
{
    // Release our copy of the shared_ptr exactly like std::shared_ptr would.
    if (!holder.control) return;
    auto* counts = reinterpret_cast<volatile long*>(holder.control);
    void** vtable = *reinterpret_cast<void***>(holder.control);
    if (InterlockedDecrement(&counts[1]) == 0) {
        reinterpret_cast<RefCountVirtualFn>(vtable[0])(holder.control, nullptr);
        if (InterlockedDecrement(&counts[2]) == 0) {
            reinterpret_cast<RefCountVirtualFn>(vtable[1])(holder.control, nullptr);
        }
    }
    holder.control = nullptr;
}

// --- SEH-guarded calls (POD locals only) --------------------------------------

int CallOpenGuarded(const char* path)
{
    __try {
        void* app = g_getApplication();
        if (!app) return 0;
        SharedPtrRaw holder = {nullptr, nullptr};
        SharedPtrRaw* sp = g_getAsrManager(app, nullptr, &holder);
        void* manager = sp ? sp->object : nullptr;
        bool ok = manager ? g_openOrActivate(manager, nullptr, path) : false;
        ReleaseShared(holder);
        return ok ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

int ReadOpenListGuarded()
{
    g_openList.count = 0;
    __try {
        void* app = g_getApplication();
        if (!app) return 0;
        SharedPtrRaw holder = {nullptr, nullptr};
        SharedPtrRaw* sp = g_getAsrManager(app, nullptr, &holder);
        uint8_t* manager = sp ? static_cast<uint8_t*>(sp->object) : nullptr;
        if (manager) {
            uint8_t* head = *reinterpret_cast<uint8_t**>(manager + kListOffset);
            uint8_t* node = head ? *reinterpret_cast<uint8_t**>(head) : nullptr;
            while (node && node != head && g_openList.count < kMaxOpenAsrs) {
                uint8_t* asr = *reinterpret_cast<uint8_t**>(node + kNodeValue);
                const char* text = asr ? *reinterpret_cast<const char**>(asr + kAsrPath) : nullptr;
                char* out = g_openList.paths[g_openList.count];
                int n = 0;
                if (text) {
                    while (n < kMaxPath - 1 && text[n]) {
                        out[n] = text[n];
                        ++n;
                    }
                }
                out[n] = 0;
                ++g_openList.count;
                node = *reinterpret_cast<uint8_t**>(node);
            }
        }
        ReleaseShared(holder);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_openList.count = 0;
        return -1;
    }
}

std::string Normalise(const std::string& p)
{
    std::string r = p;
    for (char& c : r) c = (c == '/') ? '\\' : (char)toupper((unsigned char)c);
    return r;
}

bool IsAbsolute(const std::string& p)
{
    return (p.size() > 2 && p[1] == ':') || (p.size() > 1 && p[0] == '\\' && p[1] == '\\');
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
                        if (!Contains(c, getApp, 8) || !Contains(c, getMgr, 8) || !Contains(c, open, kOpenPrologueLen)) {
                            continue;
                        }
                        if (std::memcmp(getApp, kFramePrologue, sizeof(kFramePrologue)) != 0 ||
                            std::memcmp(getMgr, kFramePrologue, sizeof(kFramePrologue)) != 0 ||
                            !MatchAt(open, kOpenPrologue, kOpenPrologueLen)) {
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

bool SamePath(const std::string& stored, const std::string& absolute)
{
    if (stored.empty()) return false;
    std::string s = Normalise(stored);
    std::string a = Normalise(absolute);
    if (s == a) return true;
    if (IsAbsolute(s)) return false;
    // Profile-relative, e.g. "\VABF\ASR\MASTER.ASR": match on the tail.
    if (s[0] != '\\') s = "\\" + s;
    return a.size() > s.size() && a.compare(a.size() - s.size(), s.size(), s) == 0;
}

bool OpenAsrPaths(std::vector<std::string>& paths)
{
    paths.clear();
    if (!g_initialised) return false;
    if (ReadOpenListGuarded() < 0) return false;
    for (int i = 0; i < g_openList.count; ++i) paths.emplace_back(g_openList.paths[i]);
    return true;
}

Result Open(const std::string& asrPath)
{
    if (!g_initialised) {
        return Result::NotAvailable;
    }

    // If EuroScope already has this ASR open (possibly under a profile-relative
    // path), pass its own spelling so it switches to that tab instead of
    // opening a second copy.
    std::string target = asrPath;
    std::vector<std::string> open;
    if (OpenAsrPaths(open)) {
        for (const std::string& p : open) {
            if (SamePath(p, asrPath)) {
                target = p;
                break;
            }
        }
    }

    int r = CallOpenGuarded(target.c_str());
    if (r < 0) {
        // Never try again after a fault.
        g_initialised = false;
        return Result::Crashed;
    }
    return r ? Result::Opened : Result::Refused;
}

} // namespace AsrOpener
