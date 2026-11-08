#include "RunwayDialog.h"

#include <windows.h>
#include <commctrl.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <vector>

namespace {

// --- What we look for in EuroScope.exe (3.2.x, x86) ---------------------------
//
// CActiveRunwayDialog::CActiveRunwayDialog(CWnd* parent)
//     push dword ptr [ebp+8]        FF 75 08
//     push 85h          ; IDD       68 85 00 00 00
//     call CDialog::CDialog         E8 ?? ?? ?? ??
//     ...
//     mov dword ptr [edi], vtable   C7 07 <vtable>
//
// Its message map has NM_CLICK on the list (1012) and BN_CLICKED on the
// "Show active airports only" check box (1116); the latter stores the box in a
// global byte:   setne byte ptr [flag]   0F 95 05 <flag>
//
// The list columns are: Airport | DEP | ARR | RWY | DEP | ARR, and a cell holds
// "ICON:1" (active for departure), "ICON:2" (active for arrival) or "ICON:0".
// OnOK reads the cells back and applies them.

const int kDialogId = 0x85;
const int kListId = 1012;
const int kActiveOnlyId = 1116;
const UINT kWmNotify = 0x004E;
const UINT kWmCommand = 0x0111;
const UINT kNmClick = 0xFFFFFFFE;

const int kSlotGetMessageMap = 12;
const int kSlotDoModal = 95;
const int kSlotOnOk = 98;
const int kSlotDeletingDtor = 1;

const size_t kBufferSize = 0x800;

enum Column { kColAirport = 0, kColAptDep = 1, kColAptArr = 2, kColRunway = 3, kColRwyDep = 4, kColRwyArr = 5 };

using CtorFn = void*(__fastcall*)(void* self, void* unused, void* parent);
using DoModalFn = INT_PTR(__fastcall*)(void* self, void* unused);
using DtorFn = void*(__fastcall*)(void* self, void* unused, unsigned flags);

CtorFn g_ctor = nullptr;
void** g_vtable = nullptr;
volatile uint8_t* g_activeOnlyFlag = nullptr;
size_t g_objectSize = 0;
bool g_ready = false;
bool g_busy = false;

alignas(16) unsigned char g_dialog[kBufferSize];

struct Section {
    uint8_t* begin = nullptr;
    uint8_t* end = nullptr;
    bool Has(const void* p, size_t n = 1) const
    {
        auto* b = static_cast<const uint8_t*>(p);
        return b >= begin && b + n <= end;
    }
};
std::vector<Section> g_code, g_data;

bool InCode(const void* p, size_t n = 1)
{
    for (auto& s : g_code)
        if (s.Has(p, n)) return true;
    return false;
}
bool InData(const void* p, size_t n = 1)
{
    for (auto& s : g_data)
        if (s.Has(p, n)) return true;
    return false;
}

void CollectSections()
{
    g_code.clear();
    g_data.clear();
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        Section s;
        s.begin = base + sec->VirtualAddress;
        s.end = s.begin + (sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData);
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) g_code.push_back(s);
        else if (sec->Characteristics & IMAGE_SCN_MEM_READ) g_data.push_back(s);
    }
}

uint32_t U32(const uint8_t* p)
{
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

// Follows "jmp rel32" stubs.
const uint8_t* Follow(const uint8_t* f)
{
    for (int i = 0; i < 4 && f && InCode(f, 5) && f[0] == 0xE9; ++i)
        f = f + 5 + static_cast<int32_t>(U32(f + 1));
    return f;
}

// Validates a candidate vtable and extracts the check-box flag address.
bool CheckVtable(void** vtable, std::string& why)
{
    if (!InData(vtable, sizeof(void*) * (kSlotOnOk + 1))) {
        why = "vtable outside data";
        return false;
    }
    // GetMessageMap: "mov eax, offset map; ret"
    const uint8_t* gm = Follow(static_cast<const uint8_t*>(vtable[kSlotGetMessageMap]));
    if (!InCode(gm, 6) || gm[0] != 0xB8 || gm[5] != 0xC3) {
        why = "GetMessageMap not recognised";
        return false;
    }
    const uint8_t* map = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(U32(gm + 1)));
    if (!InData(map, 8)) return false;
    const uint8_t* entries = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(U32(map + 4)));
    bool listClick = false;
    const uint8_t* checkHandler = nullptr;
    for (int i = 0; i < 32 && InData(entries + i * 24, 24); ++i) {
        const uint8_t* e = entries + i * 24;
        uint32_t msg = U32(e), code = U32(e + 4), id = U32(e + 8), pfn = U32(e + 20);
        if (msg == 0 && pfn == 0) break;
        // MFC stores the notification code as 16 bits (0x0000FFFE for NM_CLICK).
        if (msg == kWmNotify && (code & 0xFFFF) == (kNmClick & 0xFFFF) && id == kListId) listClick = true;
        if (msg == kWmCommand && code == 0 && id == kActiveOnlyId)
            checkHandler = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(pfn));
    }
    if (!listClick || !checkHandler || !InCode(checkHandler, 0x40)) {
        why = "message map does not match";
        return false;
    }
    // "setne byte ptr [flag]" in the check-box handler
    const uint8_t* flag = nullptr;
    for (int i = 0; i < 0x30; ++i) {
        const uint8_t* p = checkHandler + i;
        if (p[0] == 0x0F && p[1] == 0x95 && p[2] == 0x05) {
            flag = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(U32(p + 3)));
            break;
        }
    }
    if (!flag || !InData(flag)) {
        why = "check-box flag not found";
        return false;
    }
    // DoModal is MFC's (an import stub "jmp [iat]"); OnOK is EuroScope's own.
    const uint8_t* doModal = static_cast<const uint8_t*>(vtable[kSlotDoModal]);
    const uint8_t* onOk = static_cast<const uint8_t*>(vtable[kSlotOnOk]);
    if (!InCode(doModal, 6) || doModal[0] != 0xFF || doModal[1] != 0x25 || !InCode(onOk, 3) || onOk[0] != 0x55) {
        why = "DoModal/OnOK not recognised";
        return false;
    }
    // Scalar deleting destructor: "push <size>; push esi; call operator delete"
    const uint8_t* dtor = static_cast<const uint8_t*>(vtable[kSlotDeletingDtor]);
    size_t size = 0;
    for (int i = 0; InCode(dtor + i, 8) && i < 0xC0; ++i) {
        const uint8_t* p = dtor + i;
        if (p[0] == 0x68 && p[3] == 0 && p[4] == 0 && p[5] == 0x56 && p[6] == 0xE8) {
            size = U32(p + 1);
            break;
        }
    }
    if (size < 0x100 || size > kBufferSize) {
        why = "dialog size not recognised";
        return false;
    }
    g_activeOnlyFlag = const_cast<volatile uint8_t*>(flag);
    g_objectSize = size;
    return true;
}

// --- running the dialog --------------------------------------------------------

enum class Mode { Apply, Show, Dump };

struct Operation {
    Mode mode = Mode::Apply;
    std::wstring icao;
    const RunwayDialog::AirportState* wanted = nullptr;
    std::wstring dumpFile;
    bool handled = false;
    bool found = false;
};
Operation* g_op = nullptr;

std::wstring Widen(const std::string& s)
{
    std::wstring w;
    for (char c : s) w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    return w;
}

std::wstring Norm(std::wstring s)
{
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
    for (auto& c : s) c = towupper(c);
    return s;
}

std::wstring Cell(HWND lv, int item, int sub)
{
    wchar_t buf[256] = {};
    LVITEMW it{};
    it.iSubItem = sub;
    it.pszText = buf;
    it.cchTextMax = 256;
    int n = static_cast<int>(SendMessageW(lv, LVM_GETITEMTEXTW, item, reinterpret_cast<LPARAM>(&it)));
    return std::wstring(buf, n > 0 ? n : 0);
}

void SetCell(HWND lv, int item, int sub, const wchar_t* text)
{
    LVITEMW it{};
    it.iSubItem = sub;
    it.pszText = const_cast<LPWSTR>(text);
    SendMessageW(lv, LVM_SETITEMTEXTW, item, reinterpret_cast<LPARAM>(&it));
}

// The cell's first word: rows may show "VABB" or "VABB  Mumbai".
std::wstring FirstWord(const std::wstring& s)
{
    std::wstring n = Norm(s);
    size_t sp = n.find_first_of(L" \t");
    return sp == std::wstring::npos ? n : n.substr(0, sp);
}

void Hide(HWND dlg)
{
    SetWindowLongW(dlg, GWL_EXSTYLE, GetWindowLongW(dlg, GWL_EXSTYLE) | WS_EX_LAYERED | WS_EX_TOOLWINDOW);
    SetLayeredWindowAttributes(dlg, 0, 0, LWA_ALPHA);
    SetWindowPos(dlg, nullptr, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void Close(HWND dlg, int id)
{
    PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(dlg, id)));
}

// Runs right after the dialog's OnInitDialog has filled the list.
void OnDialogReady(HWND dlg)
{
    Operation& op = *g_op;
    HWND lv = GetDlgItem(dlg, kListId);
    int rows = static_cast<int>(SendMessageW(lv, LVM_GETITEMCOUNT, 0, 0));

    if (op.mode == Mode::Dump) {
        Hide(dlg);
        FILE* f = nullptr;
        if (_wfopen_s(&f, op.dumpFile.c_str(), L"w, ccs=UTF-8") == 0 && f) {
            fwprintf(f, L"rows=%d  active-only flag=%d\n", rows, g_activeOnlyFlag ? *g_activeOnlyFlag : -1);
            for (int i = 0; i < rows; ++i) {
                fwprintf(f, L"%3d", i);
                for (int c = 0; c <= 6; ++c) fwprintf(f, L" | %ls", Cell(lv, i, c).c_str());
                fwprintf(f, L"\n");
            }
            fclose(f);
        }
        op.found = true;
        Close(dlg, IDCANCEL);
        return;
    }

    if (op.mode == Mode::Show) {
        for (int i = 0; i < rows; ++i) {
            if (FirstWord(Cell(lv, i, kColAirport)) == op.icao) {
                LVITEMW st{};
                st.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
                st.state = LVIS_SELECTED | LVIS_FOCUSED;
                SendMessageW(lv, LVM_SETITEMSTATE, i, reinterpret_cast<LPARAM>(&st));
                SendMessageW(lv, LVM_ENSUREVISIBLE, i, FALSE);
                op.found = true;
                break;
            }
        }
        return; // the controller uses the dialog
    }

    // Apply: set the same cells the dialog's own click handler sets.
    Hide(dlg);
    const RunwayDialog::AirportState& want = *op.wanted;
    for (int i = 0; i < rows; ++i) {
        if (FirstWord(Cell(lv, i, kColAirport)) != op.icao) continue;
        op.found = true;
        SetCell(lv, i, kColAptDep, want.airport.dep ? L"ICON:1" : L"ICON:0");
        SetCell(lv, i, kColAptArr, want.airport.arr ? L"ICON:2" : L"ICON:0");
        std::wstring rwy = Norm(Cell(lv, i, kColRunway));
        if (rwy.empty() || Cell(lv, i, kColRwyDep).empty()) continue;
        for (const auto& r : want.runways) {
            if (Norm(Widen(r.first)) != rwy) continue;
            SetCell(lv, i, kColRwyDep, r.second.dep ? L"ICON:1" : L"ICON:0");
            SetCell(lv, i, kColRwyArr, r.second.arr ? L"ICON:2" : L"ICON:0");
        }
    }
    Close(dlg, op.found ? IDOK : IDCANCEL);
}

bool IsRunwayDialog(HWND h)
{
    wchar_t cls[16] = {};
    GetClassNameW(h, cls, 16);
    return wcscmp(cls, L"#32770") == 0 && GetDlgItem(h, kListId) && GetDlgItem(h, kActiveOnlyId);
}

LRESULT CALLBACK CallWndRetHook(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && g_op && !g_op->handled) {
        auto* c = reinterpret_cast<CWPRETSTRUCT*>(lp);
        if (c->message == WM_INITDIALOG && IsRunwayDialog(c->hwnd)) {
            g_op->handled = true;
            OnDialogReady(c->hwnd);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// Construct, DoModal, destroy - POD locals only so SEH can guard it.
int RunDialogGuarded(bool clearActiveOnly)
{
    __try {
        uint8_t saved = *g_activeOnlyFlag;
        if (clearActiveOnly) *g_activeOnlyFlag = 0; // list every airport, not only active ones
        std::memset(g_dialog, 0, sizeof(g_dialog));
        g_ctor(g_dialog, nullptr, nullptr);
        void** vt = *reinterpret_cast<void***>(g_dialog);
        INT_PTR r = reinterpret_cast<DoModalFn>(vt[kSlotDoModal])(g_dialog, nullptr);
        reinterpret_cast<DtorFn>(vt[kSlotDeletingDtor])(g_dialog, nullptr, 0);
        *g_activeOnlyFlag = saved; // the controller's own setting comes back
        return static_cast<int>(r);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -100;
    }
}

int Run(Operation& op, bool clearActiveOnly)
{
    if (!g_ready || g_busy) return -101;
    g_busy = true;
    g_op = &op;
    HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROCRET, CallWndRetHook, nullptr, GetCurrentThreadId());
    int r = hook ? RunDialogGuarded(clearActiveOnly) : -102;
    if (hook) UnhookWindowsHookEx(hook);
    g_op = nullptr;
    g_busy = false;
    if (r == -100) g_ready = false; // never again after a fault
    return r;
}

} // namespace

namespace RunwayDialog {

bool Initialise(std::string& diagnostic)
{
    if (g_ready) return true;
    CollectSections();
    if (g_code.empty() || g_data.empty()) {
        diagnostic = "could not read EuroScope.exe sections";
        return false;
    }
    const uint8_t sig[] = {0xFF, 0x75, 0x08, 0x68, kDialogId, 0x00, 0x00, 0x00, 0xE8};
    std::string why = "runway dialog not found";
    for (const Section& c : g_code) {
        for (uint8_t* p = c.begin; p + sizeof(sig) + 4 <= c.end; ++p) {
            if (p[0] != 0xFF || std::memcmp(p, sig, sizeof(sig)) != 0) continue;
            // constructor start: "push ebp; mov ebp,esp; push -1" shortly before
            uint8_t* start = nullptr;
            for (uint8_t* b = p; b > p - 0x40 && c.Has(b, 5); --b) {
                if (b[0] == 0x55 && b[1] == 0x8B && b[2] == 0xEC && b[3] == 0x6A && b[4] == 0xFF) {
                    start = b;
                    break;
                }
            }
            // "mov dword ptr [edi], vtable" shortly after
            void** vtable = nullptr;
            for (uint8_t* f = p + sizeof(sig) + 4; f < p + 0x40 && c.Has(f, 6); ++f) {
                if (f[0] == 0xC7 && f[1] == 0x07) {
                    vtable = reinterpret_cast<void**>(static_cast<uintptr_t>(U32(f + 2)));
                    break;
                }
            }
            if (!start || !vtable) continue;
            if (!CheckVtable(vtable, why)) continue;
            g_ctor = reinterpret_cast<CtorFn>(start);
            g_vtable = vtable;
            g_ready = true;
            char buf[128];
            sprintf_s(buf, "runway dialog found at EuroScope.exe+0x%X",
                      static_cast<unsigned>(start - reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr))));
            diagnostic = buf;
            return true;
        }
    }
    diagnostic = why;
    return false;
}

bool Available()
{
    return g_ready;
}

Result Apply(const std::string& icao, const AirportState& wanted)
{
    if (!g_ready) return Result::NotAvailable;
    Operation op;
    op.mode = Mode::Apply;
    op.icao = Norm(Widen(icao));
    op.wanted = &wanted;
    int r = Run(op, true);
    if (r < 0 || !op.handled) return Result::Failed;
    if (!op.found) return Result::AirportNotListed;
    return r == IDOK ? Result::Applied : Result::Failed;
}

bool Show(const std::string& icao)
{
    Operation op;
    op.mode = Mode::Show;
    op.icao = Norm(Widen(icao));
    return Run(op, false) >= 0;
}

bool Dump(const std::wstring& file)
{
    Operation op;
    op.mode = Mode::Dump;
    op.dumpFile = file;
    return Run(op, true) >= 0 && op.found;
}

} // namespace RunwayDialog
