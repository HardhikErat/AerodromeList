#include "MetarHighlight.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

// --- Signatures (EuroScope 3.2.x, x86) ---------------------------------------
//
// The Metars list builds one CMetarScreenElement per row through a small
// make_shared helper:
//
//   std::shared_ptr<CScreenElement>* __cdecl Make(std::shared_ptr<...>* out,
//                                                 const char** station,
//                                                 const RECT* row);
//
//   push ebx / mov ebx,esp / sub esp,8 / and esp,-8 / add esp,4 / push ebp
//   mov ebp,[ebx+4] / mov [esp+4],ebp / mov ebp,esp          (aligned frame)
//   ...  push 3Ch  (sizeof control block + element)
//   ...  mov eax,[ebx+10h] (row)  ...  mov eax,[ebx+0Ch] (station)
//   ...  mov dword ptr [esi], offset CMetarScreenElement::`vftable'
//
// The vftable is found from the RTTI name ".?AVCMetarScreenElement@@".

const uint8_t kPrologue[] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4, 0xF8, 0x83, 0xC4,
                             0x04, 0x55, 0x8B, 0x6B, 0x04, 0x89, 0x6C, 0x24, 0x04, 0x8B, 0xEC};
const size_t kStolen = 6; // push ebx / mov ebx,esp / sub esp,8 : position independent
const size_t kMaxBack = 0x200;
const char kTypeName[] = ".?AVCMetarScreenElement@@";

using MakeFn = void*(__cdecl*)(void* out, const char** station, const RECT* row);

struct Section {
    uint8_t* begin = nullptr;
    uint8_t* end = nullptr;
};

struct Row {
    char icao[8];
    RECT rc;
};

const int kMaxRows = 128;
Row g_rows[kMaxRows];
int g_rowCount = 0;

uint8_t* g_target = nullptr;
uint8_t g_original[kStolen];
uint8_t* g_trampoline = nullptr;
bool g_installed = false;

void CollectSections(HMODULE module, std::vector<Section>& code, std::vector<Section>& data)
{
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        Section s;
        s.begin = base + sec->VirtualAddress;
        s.end = s.begin + sec->Misc.VirtualSize;
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) code.push_back(s);
        else if (sec->Characteristics & IMAGE_SCN_MEM_READ) data.push_back(s);
    }
}

// Every 4-byte aligned... (RTTI and vftables are 4-byte aligned) slot holding `value`.
std::vector<uint8_t*> FindDword(const std::vector<Section>& sections, uint32_t value)
{
    std::vector<uint8_t*> hits;
    for (const Section& s : sections) {
        for (uint8_t* p = s.begin; p + 4 <= s.end; p += 4) {
            uint32_t v;
            std::memcpy(&v, p, 4);
            if (v == value) hits.push_back(p);
        }
    }
    return hits;
}

bool InSections(const std::vector<Section>& sections, const uint8_t* p, size_t len)
{
    for (const Section& s : sections)
        if (p >= s.begin && p + len <= s.end) return true;
    return false;
}

uint32_t FindVftable(const std::vector<Section>& data)
{
    const size_t nameLen = sizeof(kTypeName); // with the terminating zero
    for (const Section& s : data) {
        for (uint8_t* p = s.begin; p + nameLen <= s.end; ++p) {
            if (std::memcmp(p, kTypeName, nameLen) != 0) continue;
            uint32_t typeDescriptor = (uint32_t)(uintptr_t)(p - 8);
            // Complete object locator: { signature 0, offset, cdOffset, pTypeDescriptor, ... }
            for (uint8_t* ref : FindDword(data, typeDescriptor)) {
                uint8_t* col = ref - 12;
                if (!InSections(data, col, 20)) continue;
                uint32_t sig, offset;
                std::memcpy(&sig, col, 4);
                std::memcpy(&offset, col + 4, 4);
                if (sig != 0 || offset != 0) continue;
                for (uint8_t* slot : FindDword(data, (uint32_t)(uintptr_t)col)) return (uint32_t)(uintptr_t)(slot + 4);
            }
        }
    }
    return 0;
}

bool Contains(const uint8_t* from, const uint8_t* to, const uint8_t* bytes, size_t len)
{
    for (const uint8_t* p = from; p + len <= to; ++p)
        if (std::memcmp(p, bytes, len) == 0) return true;
    return false;
}

uint8_t* FindFactory(const std::vector<Section>& code, uint32_t vftable)
{
    uint8_t imm[4];
    std::memcpy(imm, &vftable, 4);
    for (const Section& s : code) {
        for (uint8_t* p = s.begin + 2; p + 4 <= s.end; ++p) {
            // mov dword ptr [reg], imm32  ->  C7 0x <imm32>
            if (p[-2] != 0xC7 || p[-1] > 0x07 || p[-1] == 0x04 || p[-1] == 0x05) continue;
            if (std::memcmp(p, imm, 4) != 0) continue;
            uint8_t* store = p - 2;
            uint8_t* limit = store - kMaxBack < s.begin ? s.begin : store - kMaxBack;
            for (uint8_t* f = store - sizeof(kPrologue); f >= limit; --f) {
                if (std::memcmp(f, kPrologue, sizeof(kPrologue)) != 0) continue;
                if (f[-1] != 0xCC) break; // not a function start: give up on this store
                const uint8_t push3C[] = {0x6A, 0x3C};
                const uint8_t rowArg[] = {0x8B, 0x43, 0x10};
                const uint8_t nameArg[] = {0x8B, 0x43, 0x0C};
                if (Contains(f, store, push3C, 2) && Contains(f, store, rowArg, 3) &&
                    Contains(f, store, nameArg, 3))
                    return f;
                break;
            }
        }
    }
    return nullptr;
}

void Record(const char** station, const RECT* row)
{
    __try {
        if (!station || !*station || !row || g_rowCount >= kMaxRows) return;
        const char* s = *station;
        Row& r = g_rows[g_rowCount];
        int n = 0;
        while (n < 7 && s[n] && s[n] != ' ') {
            char c = s[n];
            r.icao[n] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
            ++n;
        }
        r.icao[n] = 0;
        r.rc = *row;
        if (n > 0) ++g_rowCount;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void* __cdecl Hook(void* out, const char** station, const RECT* row)
{
    Record(station, row);
    return reinterpret_cast<MakeFn>(g_trampoline)(out, station, row);
}

bool WriteCode(uint8_t* at, const uint8_t* bytes, size_t len)
{
    DWORD old;
    if (!VirtualProtect(at, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(at, bytes, len);
    VirtualProtect(at, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, len);
    return true;
}

} // namespace

namespace MetarHighlight {

bool Initialise(std::string& diagnostic)
{
    if (g_installed) return true;
    std::vector<Section> code, data;
    CollectSections(GetModuleHandleW(nullptr), code, data);
    if (code.empty() || data.empty()) {
        diagnostic = "EuroScope image not readable";
        return false;
    }
    uint32_t vftable = FindVftable(data);
    if (!vftable) {
        diagnostic = "Metars list element type not found";
        return false;
    }
    uint8_t* f = FindFactory(code, vftable);
    if (!f) {
        diagnostic = "Metars list row code not found";
        return false;
    }

    g_trampoline = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_trampoline) {
        diagnostic = "out of memory";
        return false;
    }
    std::memcpy(g_trampoline, f, kStolen);
    g_trampoline[kStolen] = 0xE9;
    int32_t back = (int32_t)((f + kStolen) - (g_trampoline + kStolen + 5));
    std::memcpy(g_trampoline + kStolen + 1, &back, 4);
    FlushInstructionCache(GetCurrentProcess(), g_trampoline, 32);

    std::memcpy(g_original, f, kStolen);
    uint8_t patch[kStolen];
    patch[0] = 0xE9;
    int32_t rel = (int32_t)(reinterpret_cast<uint8_t*>(&Hook) - (f + 5));
    std::memcpy(patch + 1, &rel, 4);
    patch[5] = 0x90;
    if (!WriteCode(f, patch, kStolen)) {
        VirtualFree(g_trampoline, 0, MEM_RELEASE);
        g_trampoline = nullptr;
        diagnostic = "could not patch";
        return false;
    }
    g_target = f;
    g_installed = true;
    return true;
}

bool Available()
{
    return g_installed;
}

void Shutdown()
{
    if (!g_installed) return;
    WriteCode(g_target, g_original, kStolen);
    g_installed = false;
    // The trampoline is left allocated: a draw already inside it must still find it.
}

void BeginFrame()
{
    g_rowCount = 0;
}

bool RowRect(const std::string& icao, RECT& rc)
{
    for (int i = g_rowCount - 1; i >= 0; --i) {
        if (icao == g_rows[i].icao) {
            rc = g_rows[i].rc;
            return true;
        }
    }
    return false;
}

} // namespace MetarHighlight
