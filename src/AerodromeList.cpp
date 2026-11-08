// Aerodrome List - EuroScope plug-in
//
// A small always-available window, styled like EuroScope's own lists, showing
// every aerodrome of the loaded sector file that has a ground ASR in
// <FIR>\ASR\GRP. Clicking one opens its ASR (or switches to it if already open).
// Aerodromes can be pinned to the top and reordered by dragging; a Master
// button returns to the FIR's Master ASR.

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../sdk/EuroScopePlugIn.h"
#include "AsrIndex.h"
#include "AsrOpener.h"
#include "RunwayDialog.h"

using namespace adl;

namespace {

const char* const kPluginName = "Aerodrome List";
const char* const kPluginVersion = "1.2.2";
const char* const kPluginAuthor = "Hardhik Erat";
const char* const kPluginLicense = "Free to use";

const UINT WM_APP_INDEX_READY = WM_APP + 1;
const UINT WM_APP_OPEN_ITEM = WM_APP + 2;
const UINT WM_APP_RETURN_FOCUS = WM_APP + 3;
const UINT WM_APP_RUNWAY = WM_APP + 4; // wParam = panel row, lParam = 1 for DEP / 0 for ARR

const int IDC_FILTER = 1001;

const wchar_t* const kFrameClass = L"AerodromeListPluginWindow";
const wchar_t* const kRowsClass = L"AerodromeListPluginRows";
const wchar_t* const kBoxClass = L"AerodromeListPluginNameBox";

// --- Look: matches EuroScope's built-in lists ----------------------------------

const COLORREF kBody = RGB(30, 30, 30);
const COLORREF kStrip = RGB(38, 38, 38);
const COLORREF kBevelLight = RGB(88, 88, 88);
const COLORREF kBevelMid = RGB(69, 69, 69);
const COLORREF kBevelDark = RGB(17, 17, 17);
const COLORREF kField = RGB(22, 22, 22);
const COLORREF kButton = RGB(58, 58, 58);
const COLORREF kButtonDown = RGB(44, 44, 44);
const COLORREF kText = RGB(187, 187, 187);
const COLORREF kTextBright = RGB(215, 215, 215);
const COLORREF kTextDim = RGB(120, 122, 123);
const COLORREF kHover = RGB(43, 43, 43);
const COLORREF kSelected = RGB(55, 55, 55);
const COLORREF kPinOn = RGB(220, 200, 40); // EuroScope yellow
const COLORREF kPinOff = RGB(78, 78, 78);
const COLORREF kActive = RGB(80, 200, 110);

const int kHeaderH = 15;
const int kToolbarH = 22;
const int kRowH = 18;
const int kPinW = 20;
const int kScrollW = 8;
const int kGrip = 5;
const int kMasterW = 58;
const int kSummaryW = 46;     // active-runway column at the right of each row
const int kPanelHeaderH = 18; // runway panel title strip (even, so its 12px buttons centre exactly)
const int kPanelRowH = 17;
const int kChipW = 34;
const int kChipH = 13;
const int kChipDepX = 50;     // chip positions inside the runway panel
const int kChipArrX = 88;
const int kBoxPad = 6;       // right-click name box
const int kBoxLineH = 18;

const COLORREF kRunwayText = RGB(110, 190, 230);
const COLORREF kDepOn = RGB(46, 125, 70);
const COLORREF kArrOn = RGB(40, 95, 150);

const UINT_PTR kSaveTimer = 1;
const UINT kSaveDelayMs = 400; // changes are written this long after they stop

// ---------------------------------------------------------------------------
// Settings (AerodromeList.ini next to the DLL)
// ---------------------------------------------------------------------------

struct Settings {
    std::wstring file;
    RECT rect{0, 0, 0, 0};
    bool topmost = false;
    bool collapsed = false;
    int scrollTop = 0;       // list scroll position in pixels
    std::string runwayPanel; // aerodrome shown in the runway panel ("" = closed)
    std::vector<std::wstring> extraFolders;
    std::vector<std::string> pins;

    void Load()
    {
        wchar_t buf[4096];
        rect.left = GetPrivateProfileIntW(L"Window", L"X", INT_MIN, file.c_str());
        rect.top = GetPrivateProfileIntW(L"Window", L"Y", INT_MIN, file.c_str());
        int w = GetPrivateProfileIntW(L"Window", L"Width", 0, file.c_str());
        int h = GetPrivateProfileIntW(L"Window", L"Height", 0, file.c_str());
        rect.right = rect.left + w;
        rect.bottom = rect.top + h;
        topmost = GetPrivateProfileIntW(L"Window", L"AlwaysOnTop", 0, file.c_str()) != 0;
        collapsed = GetPrivateProfileIntW(L"Window", L"Collapsed", 0, file.c_str()) != 0;
        scrollTop = std::max(0, (int)GetPrivateProfileIntW(L"List", L"ScrollTop", 0, file.c_str()));
        GetPrivateProfileStringW(L"List", L"RunwayPanel", L"", buf, 16, file.c_str());
        runwayPanel = Upper(Narrow(buf));

        GetPrivateProfileStringW(L"Folders", L"AsrFolders", L"", buf, 4096, file.c_str());
        for (const std::wstring& part : Split(buf)) {
            std::wstring p = part;
            if (p.size() > 1 && p[1] != L':' && p[0] != L'\\') p = ParentDir(file) + L"\\" + p;
            extraFolders.push_back(p);
        }

        GetPrivateProfileStringW(L"Pins", L"Order", L"", buf, 4096, file.c_str());
        for (const std::wstring& part : SplitAny(buf, L",; ")) {
            std::string icao = Upper(Narrow(part));
            if (!icao.empty() && std::find(pins.begin(), pins.end(), icao) == pins.end()) pins.push_back(icao);
        }
    }

    void SaveWindow() const
    {
        auto put = [&](const wchar_t* key, int v) {
            WritePrivateProfileStringW(L"Window", key, std::to_wstring(v).c_str(), file.c_str());
        };
        put(L"X", rect.left);
        put(L"Y", rect.top);
        put(L"Width", rect.right - rect.left);
        put(L"Height", rect.bottom - rect.top);
        put(L"AlwaysOnTop", topmost ? 1 : 0);
        put(L"Collapsed", collapsed ? 1 : 0);
        WritePrivateProfileStringW(L"Window", L"Visible", nullptr, file.c_str()); // no longer used
        WritePrivateProfileStringW(L"List", L"ScrollTop", std::to_wstring(scrollTop).c_str(), file.c_str());
        WritePrivateProfileStringW(L"List", L"RunwayPanel", Widen(runwayPanel).c_str(), file.c_str());
    }

    void SavePins() const
    {
        std::wstring all;
        for (const std::string& p : pins) all += (all.empty() ? L"" : L",") + Widen(p);
        WritePrivateProfileStringW(L"Pins", L"Order", all.c_str(), file.c_str());
    }

    bool HasRect() const
    {
        return rect.left != INT_MIN && rect.top != INT_MIN && rect.right - rect.left > 50 &&
               rect.bottom - rect.top > 40;
    }

    static std::vector<std::wstring> SplitAny(const std::wstring& all, const wchar_t* seps)
    {
        std::vector<std::wstring> out;
        size_t pos = 0;
        while (pos <= all.size()) {
            size_t sep = all.find_first_of(seps, pos);
            if (sep == std::wstring::npos) sep = all.size();
            std::wstring part = all.substr(pos, sep - pos);
            while (!part.empty() && iswspace(part.back())) part.pop_back();
            while (!part.empty() && iswspace(part.front())) part.erase(part.begin());
            if (!part.empty()) out.push_back(part);
            pos = sep + 1;
        }
        return out;
    }
    static std::vector<std::wstring> Split(const std::wstring& all) { return SplitAny(all, L";"); }
};

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

void Fill(HDC dc, const RECT& r, COLORREF c)
{
    SetBkColor(dc, c);
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &r, nullptr, 0, nullptr);
}

void HLine(HDC dc, int x0, int x1, int y, COLORREF c)
{
    RECT r{x0, y, x1, y + 1};
    Fill(dc, r, c);
}

void VLine(HDC dc, int x, int y0, int y1, COLORREF c)
{
    RECT r{x, y0, x + 1, y1};
    Fill(dc, r, c);
}

// EuroScope style 3D edge: light top/left, dark bottom/right (reversed when sunken).
void Bevel(HDC dc, const RECT& r, COLORREF face, bool raised)
{
    Fill(dc, r, face);
    COLORREF tl = raised ? kBevelLight : kBevelDark;
    COLORREF br = raised ? kBevelDark : kBevelLight;
    HLine(dc, r.left, r.right, r.top, tl);
    VLine(dc, r.left, r.top, r.bottom, tl);
    HLine(dc, r.left, r.right, r.bottom - 1, br);
    VLine(dc, r.right - 1, r.top, r.bottom, br);
}

void Text(HDC dc, const std::wstring& s, RECT r, COLORREF c, UINT flags)
{
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s.c_str(), (int)s.size(), &r, flags | DT_SINGLELINE | DT_NOPREFIX);
}

// Small push-pin: round head with a needle.
void DrawPin(HDC dc, int cx, int cy, bool pinned, bool hot)
{
    COLORREF c = pinned ? kPinOn : (hot ? kText : kPinOff);
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HBRUSH brush = pinned ? CreateSolidBrush(c) : (HBRUSH)GetStockObject(NULL_BRUSH);
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, brush);
    Ellipse(dc, cx - 3, cy - 6, cx + 4, cy + 1);
    MoveToEx(dc, cx, cy + 1, nullptr);
    LineTo(dc, cx, cy + 6);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
    if (pinned) DeleteObject(brush);
}

void DrawTriangle(HDC dc, int cx, int cy, bool up, COLORREF c)
{
    POINT p[3];
    if (up) {
        p[0] = {cx - 3, cy + 2};
        p[1] = {cx + 3, cy + 2};
        p[2] = {cx, cy - 2};
    } else {
        p[0] = {cx - 3, cy - 2};
        p[1] = {cx + 3, cy - 2};
        p[2] = {cx, cy + 2};
    }
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HBRUSH brush = CreateSolidBrush(c);
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, brush);
    Polygon(dc, p, 3);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
    DeleteObject(brush);
}

bool FontInstalled(const wchar_t* face)
{
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, face);
    bool found = false;
    HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(
        dc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lp) -> int {
            *reinterpret_cast<bool*>(lp) = true;
            return 0;
        },
        reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, dc);
    return found;
}

HFONT MakeFont(const wchar_t* face, int height, int weight = FW_NORMAL)
{
    return CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, face);
}

// ---------------------------------------------------------------------------
// Plug-in
// ---------------------------------------------------------------------------

struct RunwayEnd {
    std::string name; // "09", "27L"
    bool dep = false;
    bool arr = false;
};

// Active airport/runway state of one sector aerodrome, read through the SDK.
struct SectorAirport {
    bool dep = false; // airport active for departures
    bool arr = false; // airport active for arrivals
    std::vector<RunwayEnd> runways;
};

struct Airport {
    std::string icao;
    std::string name;
    std::vector<std::wstring> asrs;
};

// What a click in the runway panel changes.
struct RunwayToggle {
    int row = -1;     // 0 = the airport itself, 1.. = runway end
    bool dep = true;  // DEP or ARR column
};

class AerodromeListPlugin : public EuroScopePlugIn::CPlugIn
{
public:
    AerodromeListPlugin();
    ~AerodromeListPlugin() override;

    void OnTimer(int counter) override;
    bool OnCompileCommand(const char* command) override;
    void OnAirportRunwayActivityChanged() override;

private:
    enum class Button { None, Collapse, Master, PanelClose, PanelDialog };

    // window procedures
    static LRESULT CALLBACK FrameProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK RowsProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK FilterProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleFrame(UINT, WPARAM, LPARAM);
    LRESULT HandleRows(UINT, WPARAM, LPARAM);
    LRESULT HandleFilter(UINT, WPARAM, LPARAM);

    // frame
    void EnsureWindow();
    void CreateResources();
    void Layout();
    void PaintFrame(HDC dc);
    Button ButtonAt(POINT pt) const;
    void SetCollapsed(bool collapsed);
    void SetVisible(bool visible);
    void SaveRect();
    void QueueSave();  // save window/list state shortly after a change
    void SaveNow();    // save everything immediately
    HWND FindEuroScopeWindow() const;

    // rows
    void PaintRows(HDC dc, const RECT& rc);
    int RowCount() const { return (int)visible_.size(); }
    int ContentHeight() const { return RowCount() * kRowH; }
    bool HasScrollbar() const;
    RECT ThumbRect() const;
    int ItemAt(int y) const;
    void ScrollTo(int top);
    void TryRestoreScroll();
    void EnsureVisible(int item);
    void InvalidateRows() { if (rows_) InvalidateRect(rows_, nullptr, FALSE); }
    void ShowNameBox(int item, POINT screenPt);
    void CloseNameBox();
    static LRESULT CALLBACK NameBoxProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleNameBox(HWND, UINT, WPARAM, LPARAM);

    // data
    void StartIndexing();
    void ApplyIndex();
    bool RefreshSectorAirports(bool force);
    void RebuildList();
    bool IsPinned(const std::string& icao) const;
    void TogglePin(std::string icao);
    void ApplyDrop(int from, int slot);
    void OpenItem(int item, int asrChoice);
    void OpenIcao(std::string icao, int asrChoice);
    void OpenMaster();
    std::wstring ChooseMaster() const;

    // active runways
    const SectorAirport* SectorInfo(const std::string& icao) const;
    std::wstring RunwaySummary(const std::string& icao) const;
    void SetRunwayPanel(const std::string& icao);
    int RunwayPanelHeight() const;
    void PaintRunwayPanel(HDC dc);
    bool RunwayPanelHit(POINT pt, RunwayToggle& hit) const;
    void ToggleRunway(const RunwayToggle& t);
    bool SetActiveRunways(const std::string& icao, const std::string& spec);
    bool ApplyRunways(const std::string& icao, const RunwayDialog::AirportState& wanted);
    bool OpenPath(const std::wstring& path, const std::string& what);
    void WriteInfo();
    void Message(const std::string& text, bool flash = false);

    HWND hwnd_ = nullptr;
    HWND filter_ = nullptr;
    HWND rows_ = nullptr;
    HWND owner_ = nullptr;
    WNDPROC filterDefault_ = nullptr;
    HFONT titleFont_ = nullptr;
    HFONT listFont_ = nullptr;
    HFONT buttonFont_ = nullptr;
    HBRUSH fieldBrush_ = nullptr;
    std::wstring listFace_;

    // frame layout & state
    RECT headerRc_{}, collapseRc_{}, masterRc_{}, fieldRc_{}, rowsRc_{};
    RECT panelRc_{}, panelCloseRc_{}, panelDialogRc_{}; // runway panel (empty when closed)
    bool runwayBusy_ = false;

    // right-click name box
    HWND nameBox_ = nullptr;
    struct {
        std::string icao;
        std::wstring name;
        std::vector<std::wstring> asrs; // listed only when there is more than one
        int hover = -1;
    } box_;
    Button pressed_ = Button::None;
    Button hot_ = Button::None;
    int expandedHeight_ = 0;

    // rows state
    int top_ = 0;           // scroll offset in pixels
    int hover_ = -1;
    bool hoverPin_ = false;
    int sel_ = -1;
    int pressItem_ = -1;
    bool pressOnPin_ = false;
    POINT pressPt_{};
    bool dragging_ = false;
    int dropSlot_ = -1;
    bool thumbDrag_ = false;
    int thumbGrab_ = 0;
    bool trackingMouse_ = false;

    Settings settings_;
    std::wstring pluginDir_;

    std::vector<std::string> sectorAirports_; // ICAO codes, sorted
    std::map<std::string, SectorAirport> sectorInfo_; // active airport/runway state per ICAO
    std::string sectorSignature_;
    std::vector<Airport> airports_; // sector aerodromes that have an ASR
    std::vector<int> visible_;      // indices into airports_: pinned (in pin order) first, then A-Z
    int pinnedVisible_ = 0;
    std::string lastOpenedIcao_;

    std::thread indexThread_;
    std::mutex indexMutex_;
    std::unique_ptr<AsrIndex> pendingIndex_;
    std::atomic<bool> indexReady_{false};
    std::atomic<HWND> notifyHwnd_{nullptr};
    AsrIndex index_;
    bool indexLoaded_ = false;
    int reindexAttempts_ = 0;
    bool scrollRestored_ = false; // saved scroll position applied once the list has content

    bool openerReady_ = false;
    std::string openerDiagnostic_;
    std::string runwayDiagnostic_;
};

AerodromeListPlugin* g_plugin = nullptr;

HINSTANCE ThisModule()
{
    HINSTANCE inst = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ThisModule), &inst);
    return inst;
}

AerodromeListPlugin::AerodromeListPlugin()
    : CPlugIn(EuroScopePlugIn::COMPATIBILITY_CODE, kPluginName, kPluginVersion, kPluginAuthor, kPluginLicense)
{
    pluginDir_ = ParentDir(ModulePath());
    settings_.file = pluginDir_ + L"\\AerodromeList.ini";
    settings_.Load();

    openerReady_ = AsrOpener::Initialise(openerDiagnostic_);
    RunwayDialog::Initialise(runwayDiagnostic_);

    StartIndexing();
    RefreshSectorAirports(true);
    EnsureWindow();

    if (!openerReady_) {
        Message("Could not hook EuroScope's ASR loader (" + openerDiagnostic_ +
                "). Clicking an aerodrome will copy its ASR path to the clipboard instead.");
    }
}

AerodromeListPlugin::~AerodromeListPlugin()
{
    notifyHwnd_ = nullptr;
    if (indexThread_.joinable()) indexThread_.join();
    CloseNameBox();
    if (hwnd_ && IsWindow(hwnd_)) {
        DestroyWindow(hwnd_); // WM_DESTROY writes the final state
    } else {
        SaveNow();            // window already gone (EuroScope closed it): write what we have
    }
    hwnd_ = nullptr;
    // The classes point at code in this DLL; unregister them so a reload re-registers cleanly.
    UnregisterClassW(kFrameClass, ThisModule());
    UnregisterClassW(kRowsClass, ThisModule());
    UnregisterClassW(kBoxClass, ThisModule());
    for (HGDIOBJ o : {(HGDIOBJ)titleFont_, (HGDIOBJ)listFont_, (HGDIOBJ)buttonFont_, (HGDIOBJ)fieldBrush_})
        if (o) DeleteObject(o);
}

void AerodromeListPlugin::Message(const std::string& text, bool flash)
{
    DisplayUserMessage(kPluginName, kPluginName, text.c_str(), true, true, false, flash, false);
}

// --- data -------------------------------------------------------------------

void AerodromeListPlugin::StartIndexing()
{
    if (indexThread_.joinable()) indexThread_.join();
    indexReady_ = false;
    std::wstring dir = pluginDir_;
    std::vector<std::wstring> extra = settings_.extraFolders;
    indexThread_ = std::thread([this, dir, extra]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        auto result = std::make_unique<AsrIndex>(BuildIndex(dir, extra));
        std::vector<std::wstring> toWarm;
        for (auto& kv : result->asrByIcao)
            for (auto& p : kv.second) toWarm.push_back(p);
        for (auto& kv : result->masterByGrp) toWarm.push_back(kv.second);
        {
            std::lock_guard<std::mutex> lock(indexMutex_);
            pendingIndex_ = std::move(result);
        }
        indexReady_ = true;
        if (HWND h = notifyHwnd_.load()) PostMessageW(h, WM_APP_INDEX_READY, 0, 0);
        // Pull every ASR into the OS file cache so a click loads from memory.
        for (auto& p : toWarm) PrewarmFile(p);
    });
}

void AerodromeListPlugin::ApplyIndex()
{
    if (!indexReady_) return;
    std::unique_ptr<AsrIndex> idx;
    {
        std::lock_guard<std::mutex> lock(indexMutex_);
        idx = std::move(pendingIndex_);
    }
    indexReady_ = false;
    if (!idx) return;
    index_ = std::move(*idx);
    indexLoaded_ = true;
    RebuildList();
    if (index_.grpFolders.empty() && reindexAttempts_ >= 3) {
        Message("No <FIR>\\ASR\\GRP folder found near the plug-in. Add one under [Folders] AsrFolders= in "
                "AerodromeList.ini.");
    }
}

bool AerodromeListPlugin::RefreshSectorAirports(bool force)
{
    // First word, upper case: sector names can be "VABB" or "VABB Mumbai".
    auto code = [](const char* n) {
        std::string s = Upper(n ? n : "");
        size_t sp = s.find_first_of(" \t");
        return sp == std::string::npos ? s : s.substr(0, sp);
    };

    std::vector<std::string> codes;
    std::map<std::string, SectorAirport> info;
    for (EuroScopePlugIn::CSectorElement e = SectorFileElementSelectFirst(EuroScopePlugIn::SECTOR_ELEMENT_AIRPORT);
         e.IsValid(); e = SectorFileElementSelectNext(e, EuroScopePlugIn::SECTOR_ELEMENT_AIRPORT)) {
        std::string c = code(e.GetName());
        if (c.empty()) continue;
        codes.push_back(c);
        SectorAirport& a = info[c];
        a.dep = e.IsElementActive(true);
        a.arr = e.IsElementActive(false);
    }
    // Runway ends, in sector file order (the same order as EuroScope's runway dialog).
    for (EuroScopePlugIn::CSectorElement e = SectorFileElementSelectFirst(EuroScopePlugIn::SECTOR_ELEMENT_RUNWAY);
         e.IsValid(); e = SectorFileElementSelectNext(e, EuroScopePlugIn::SECTOR_ELEMENT_RUNWAY)) {
        std::string c = code(e.GetAirportName());
        if (c.empty()) continue;
        SectorAirport& a = info[c];
        for (int i = 0; i < 2; ++i) {
            std::string name = code(e.GetRunwayName(i));
            if (name.empty()) continue;
            RunwayEnd r;
            r.name = name;
            r.dep = e.IsElementActive(true, i);
            r.arr = e.IsElementActive(false, i);
            a.runways.push_back(r);
        }
    }
    std::sort(codes.begin(), codes.end());
    codes.erase(std::unique(codes.begin(), codes.end()), codes.end());

    std::string sig;
    for (auto& c : codes) sig += c + ",";
    for (auto& kv : info) {
        sig += kv.first + (kv.second.dep ? "D" : "") + (kv.second.arr ? "A" : "");
        for (auto& r : kv.second.runways) sig += r.name + (r.dep ? "d" : "") + (r.arr ? "a" : "") + ";";
    }
    if (!force && sig == sectorSignature_) return false;
    sectorSignature_ = sig;
    sectorAirports_ = std::move(codes);
    sectorInfo_ = std::move(info);
    RebuildList();
    if (hwnd_) {
        Layout(); // the runway panel's height may have changed
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    return true;
}

const SectorAirport* AerodromeListPlugin::SectorInfo(const std::string& icao) const
{
    auto f = sectorInfo_.find(icao);
    return f == sectorInfo_.end() ? nullptr : &f->second;
}

// "27" when the same runways are used both ways, else "27/14" (departure/arrival).
std::wstring AerodromeListPlugin::RunwaySummary(const std::string& icao) const
{
    const SectorAirport* a = SectorInfo(icao);
    if (!a) return L"";
    std::string dep, arr;
    for (const RunwayEnd& r : a->runways) {
        if (r.dep) dep += (dep.empty() ? "" : ",") + r.name;
        if (r.arr) arr += (arr.empty() ? "" : ",") + r.name;
    }
    if (dep.empty() && arr.empty()) return L"";
    if (dep == arr) return Widen(dep);
    return Widen((dep.empty() ? "-" : dep) + "/" + (arr.empty() ? "-" : arr));
}

bool AerodromeListPlugin::IsPinned(const std::string& icao) const
{
    return std::find(settings_.pins.begin(), settings_.pins.end(), icao) != settings_.pins.end();
}

void AerodromeListPlugin::RebuildList()
{
    std::string selIcao = (sel_ >= 0 && sel_ < RowCount()) ? airports_[visible_[sel_]].icao : "";

    airports_.clear();
    for (const std::string& icao : sectorAirports_) {
        auto f = index_.asrByIcao.find(icao);
        if (f == index_.asrByIcao.end() || f->second.empty()) continue; // only aerodromes that have an ASR
        Airport a;
        a.icao = icao;
        auto n = index_.airportNames.find(icao);
        if (n != index_.airportNames.end()) a.name = n->second;
        a.asrs = f->second;
        airports_.push_back(std::move(a));
    }

    wchar_t text[64] = {};
    if (filter_) GetWindowTextW(filter_, text, 64);
    // Only the first word filters: "VABB 27" lists VABB (the rest is a runway).
    std::string needle = Upper(Narrow(text));
    while (!needle.empty() && needle.front() == ' ') needle.erase(needle.begin());
    if (needle.find(' ') != std::string::npos) needle = needle.substr(0, needle.find(' '));
    auto matches = [&](const Airport& a) {
        return needle.empty() || a.icao.find(needle) != std::string::npos ||
               Upper(a.name).find(needle) != std::string::npos;
    };

    visible_.clear();
    for (const std::string& pin : settings_.pins) {
        for (int i = 0; i < (int)airports_.size(); ++i) {
            if (airports_[i].icao == pin && matches(airports_[i])) visible_.push_back(i);
        }
    }
    pinnedVisible_ = (int)visible_.size();
    for (int i = 0; i < (int)airports_.size(); ++i) {
        if (!IsPinned(airports_[i].icao) && matches(airports_[i])) visible_.push_back(i);
    }

    sel_ = -1;
    for (int i = 0; i < RowCount(); ++i)
        if (airports_[visible_[i]].icao == selIcao) sel_ = i;
    hover_ = -1;
    ScrollTo(top_);
    TryRestoreScroll();
    InvalidateRows();
}

void AerodromeListPlugin::TogglePin(std::string icao)
{
    auto& pins = settings_.pins;
    auto it = std::find(pins.begin(), pins.end(), icao);
    if (it != pins.end()) {
        pins.erase(it);
    } else {
        pins.push_back(icao);
    }
    settings_.SavePins();
    RebuildList();
}

void AerodromeListPlugin::ApplyDrop(int from, int slot)
{
    if (from < 0 || from >= pinnedVisible_ || slot < 0 || slot == from || slot == from + 1) return;

    auto& pins = settings_.pins;
    std::string moving = airports_[visible_[from]].icao;
    bool after = slot >= pinnedVisible_;
    std::string anchor = airports_[visible_[after ? pinnedVisible_ - 1 : slot]].icao;

    pins.erase(std::find(pins.begin(), pins.end(), moving));
    auto pos = std::find(pins.begin(), pins.end(), anchor);
    if (after && pos != pins.end()) ++pos;
    pins.insert(pos, moving);
    settings_.SavePins();

    RebuildList();
    for (int i = 0; i < RowCount(); ++i)
        if (airports_[visible_[i]].icao == moving) sel_ = i;
    InvalidateRows();
}

bool AerodromeListPlugin::OpenPath(const std::wstring& path, const std::string& what)
{
    std::string p = Narrow(path);
    AsrOpener::Result r = AsrOpener::Open(p);
    if (r == AsrOpener::Result::Opened) {
        PostMessageW(hwnd_, WM_APP_RETURN_FOCUS, 0, 0);
        return true;
    }

    // Fallback: put the path on the clipboard so it can be pasted in Open SCT > Open...
    if (OpenClipboard(hwnd_)) {
        EmptyClipboard();
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (path.size() + 1) * sizeof(wchar_t));
        if (mem) {
            memcpy(GlobalLock(mem), path.c_str(), (path.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(mem);
            SetClipboardData(CF_UNICODETEXT, mem);
        }
        CloseClipboard();
    }
    if (r == AsrOpener::Result::Refused) {
        Message("EuroScope could not open the " + what + " ASR " + p + " (path copied to clipboard).");
    } else if (r == AsrOpener::Result::Crashed) {
        Message("Opening the ASR failed inside EuroScope; direct loading is now disabled for this session. "
                "Path copied to clipboard: " + p);
    } else {
        Message("Direct ASR loading is unavailable in this EuroScope version. Path copied to clipboard: " + p);
    }
    return false;
}

void AerodromeListPlugin::OpenIcao(std::string icao, int asrChoice)
{
    auto f = index_.asrByIcao.find(icao);
    if (f == index_.asrByIcao.end() || f->second.empty()) {
        Message("No ASR found for " + icao + ".");
        return;
    }
    const auto& asrs = f->second;
    int choice = (asrChoice >= 0 && asrChoice < (int)asrs.size()) ? asrChoice : 0;
    if (OpenPath(asrs[choice], icao)) {
        lastOpenedIcao_ = icao;
        InvalidateRows();
    }
}

void AerodromeListPlugin::OpenItem(int item, int asrChoice)
{
    if (item < 0 || item >= RowCount()) return;
    OpenIcao(airports_[visible_[item]].icao, asrChoice);
}

// The Master ASR of the FIR the loaded sector belongs to: the GRP folder that
// holds most of this sector's aerodrome ASRs decides which FIR that is.
std::wstring AerodromeListPlugin::ChooseMaster() const
{
    std::map<std::wstring, int> votes;
    for (const Airport& a : airports_) votes[ParentDir(a.asrs.front())]++;
    std::wstring best;
    int bestVotes = -1;
    for (const auto& kv : votes) {
        auto m = index_.masterByGrp.find(kv.first);
        if (m != index_.masterByGrp.end() && kv.second > bestVotes) {
            best = m->second;
            bestVotes = kv.second;
        }
    }
    if (best.empty()) {
        for (const std::wstring& grp : index_.grpFolders) {
            auto m = index_.masterByGrp.find(grp);
            if (m != index_.masterByGrp.end()) return m->second;
        }
    }
    return best;
}

void AerodromeListPlugin::OpenMaster()
{
    std::wstring master = ChooseMaster();
    if (master.empty()) {
        // No Master ASR on disk next to the GRP folders: fall back to an open tab called "...Master...".
        std::vector<std::string> open;
        if (AsrOpener::OpenAsrPaths(open)) {
            for (const std::string& p : open) {
                if (Upper(p).find("MASTER") != std::string::npos) {
                    master = Widen(p);
                    break;
                }
            }
        }
    }
    if (master.empty()) {
        Message("No Master ASR found (looked for <FIR>\\ASR\\*Master*.asr).");
        return;
    }
    if (OpenPath(master, "Master")) {
        lastOpenedIcao_.clear();
        InvalidateRows();
    }
}

void AerodromeListPlugin::WriteInfo()
{
    std::wstring file = pluginDir_ + L"\\AerodromeList-info.txt";
    FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"w") != 0 || !f) return;
    fprintf(f, "Aerodrome List %s\nASR loader: %s\nRunway dialog: %s\n\nGRP folders:\n", kPluginVersion,
            openerDiagnostic_.c_str(), runwayDiagnostic_.c_str());
    for (auto& g : index_.grpFolders) {
        auto m = index_.masterByGrp.find(g);
        fprintf(f, "  %s\n    master: %s\n", Narrow(g).c_str(),
                m == index_.masterByGrp.end() ? "(none)" : Narrow(m->second).c_str());
    }
    fprintf(f, "\nMaster for this sector: %s\n", Narrow(ChooseMaster()).c_str());
    fprintf(f, "\nASRs open in EuroScope:\n");
    std::vector<std::string> open;
    if (AsrOpener::OpenAsrPaths(open)) {
        for (auto& p : open) fprintf(f, "  %s\n", p.c_str());
    } else {
        fprintf(f, "  (unavailable)\n");
    }
    fprintf(f, "\nListed aerodromes: %d, pinned: %d\n", (int)airports_.size(), (int)settings_.pins.size());
    fclose(f);
    Message("Diagnostics written to " + Narrow(file));
}

// --- EuroScope callbacks ------------------------------------------------------

void AerodromeListPlugin::OnTimer(int counter)
{
    EnsureWindow();
    if (owner_ == nullptr || !IsWindow(owner_)) {
        HWND es = FindEuroScopeWindow();
        if (es && hwnd_) {
            owner_ = es;
            SetWindowLongPtrW(hwnd_, GWLP_HWNDPARENT, (LONG_PTR)es);
        }
    }
    if (indexReady_) ApplyIndex();
    // Plug-ins listed after this one in the profile load later; if nothing was
    // found at start-up, look again once they are all in.
    if (indexLoaded_ && index_.grpFolders.empty() && reindexAttempts_ < 3 && counter % 5 == 0) {
        ++reindexAttempts_;
        StartIndexing();
    }
    if (counter % 2 == 0) RefreshSectorAirports(false);
}

void AerodromeListPlugin::OnAirportRunwayActivityChanged()
{
    RefreshSectorAirports(false);
}

bool AerodromeListPlugin::OnCompileCommand(const char* command)
{
    std::string cmd = Upper(command ? command : "");
    while (!cmd.empty() && isspace((unsigned char)cmd.back())) cmd.pop_back();
    if (cmd != ".ADL" && cmd != ".AERODROMES" && cmd.rfind(".ADL ", 0) != 0) return false;

    std::string arg = cmd.size() > 5 ? cmd.substr(5) : "";
    if (arg == "RELOAD") {
        StartIndexing();
        RefreshSectorAirports(true);
        Message("Re-indexing ASR folders...");
    } else if (arg == "TOP") {
        settings_.topmost = !settings_.topmost;
        if (hwnd_)
            SetWindowPos(hwnd_, settings_.topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        settings_.SaveWindow();
        Message(settings_.topmost ? "Window is now always on top of all apps." : "Window stays above EuroScope only.");
    } else if (arg == "MASTER") {
        OpenMaster();
    } else if (arg == "INFO") {
        WriteInfo();
    } else if (arg.rfind("RWY", 0) == 0 && (arg.size() == 3 || arg[3] == ' ')) {
        // ".adl rwy VABB 27", ".adl rwy VABB 27/14", ".adl rwy VABB off", ".adl rwy VABB" (panel)
        std::string rest = arg.size() > 4 ? arg.substr(4) : "";
        size_t sp = rest.find(' ');
        std::string icao = rest.substr(0, sp);
        std::string spec = sp == std::string::npos ? "" : rest.substr(sp + 1);
        if (icao.empty()) {
            Message("Usage: .adl rwy VABB 27  |  .adl rwy VABB 27/14  |  .adl rwy VABB off");
        } else if (spec.empty()) {
            if (SectorInfo(icao)) SetRunwayPanel(icao);
            else Message(icao + " is not an aerodrome of the loaded sector file.");
        } else {
            SetActiveRunways(icao, spec);
        }
    } else if (arg == "RWYDUMP") {
        std::wstring file = pluginDir_ + L"\\AerodromeList-runways.txt";
        Message(RunwayDialog::Dump(file) ? "Runway dialog rows written to " + Narrow(file)
                                          : "Runway dialog unavailable: " + runwayDiagnostic_);
    } else if (arg.empty()) {
        SetVisible(!(hwnd_ && IsWindowVisible(hwnd_)));
    } else {
        // ".adl VABB" opens that aerodrome's ASR
        OpenIcao(arg, -1);
    }
    return true;
}

// --- window: frame -------------------------------------------------------------

HWND AerodromeListPlugin::FindEuroScopeWindow() const
{
    struct Ctx {
        HWND self;
        HWND found;
    } ctx{hwnd_, nullptr};
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            if (h == c->self || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
            wchar_t title[64] = {};
            GetWindowTextW(h, title, 64);
            if (wcsncmp(title, L"EuroScope", 9) != 0) return TRUE;
            c->found = h;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

void AerodromeListPlugin::CreateResources()
{
    listFace_ = FontInstalled(L"EuroScope") ? L"EuroScope" : L"Consolas";
    titleFont_ = CreateFontW(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Tahoma");
    listFont_ = MakeFont(listFace_.c_str(), 14);
    buttonFont_ = MakeFont(listFace_.c_str(), 13);
    fieldBrush_ = CreateSolidBrush(kField);
}

void AerodromeListPlugin::EnsureWindow()
{
    if (hwnd_) return;
    HINSTANCE inst = ThisModule();

    WNDCLASSEXW wc{sizeof(wc)};
    if (!GetClassInfoExW(inst, kFrameClass, &wc)) {
        wc = WNDCLASSEXW{sizeof(wc)};
        wc.lpfnWndProc = &AerodromeListPlugin::FrameProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kFrameClass;
        RegisterClassExW(&wc);
    }
    if (!GetClassInfoExW(inst, kRowsClass, &wc)) {
        wc = WNDCLASSEXW{sizeof(wc)};
        wc.lpfnWndProc = &AerodromeListPlugin::RowsProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kRowsClass;
        RegisterClassExW(&wc);
    }
    if (!GetClassInfoExW(inst, kBoxClass, &wc)) {
        wc = WNDCLASSEXW{sizeof(wc)};
        wc.lpfnWndProc = &AerodromeListPlugin::NameBoxProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kBoxClass;
        wc.style = CS_DROPSHADOW;
        RegisterClassExW(&wc);
    }

    CreateResources();
    owner_ = FindEuroScopeWindow();

    RECT r = settings_.rect;
    // Ignore a saved position that is no longer on any monitor (e.g. a display was removed).
    bool onScreen = settings_.HasRect() && MonitorFromRect(&r, MONITOR_DEFAULTTONULL) != nullptr;
    if (!onScreen) {
        HMONITOR mon = MonitorFromWindow(owner_ ? owner_ : GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(mon, &mi);
        int w = 250, h = 380;
        r = {mi.rcWork.right - w - 24, mi.rcWork.top + 140, mi.rcWork.right - 24, mi.rcWork.top + 140 + h};
    }
    expandedHeight_ = r.bottom - r.top;

    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | (settings_.topmost ? WS_EX_TOPMOST : 0), kFrameClass, L"Aerodromes",
                            WS_POPUP | WS_CLIPCHILDREN, r.left, r.top, r.right - r.left, r.bottom - r.top, owner_,
                            nullptr, inst, this);
    if (!hwnd_) return;

    filter_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_UPPERCASE, 0, 0, 10, 10,
                              hwnd_, (HMENU)(INT_PTR)IDC_FILTER, inst, nullptr);
    SendMessageW(filter_, WM_SETFONT, (WPARAM)listFont_, TRUE);
    SendMessageW(filter_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(0, 0));
    SendMessageW(filter_, EM_SETLIMITTEXT, 40, 0);
    SetWindowLongPtrW(filter_, GWLP_USERDATA, (LONG_PTR)this);
    filterDefault_ = (WNDPROC)SetWindowLongPtrW(filter_, GWLP_WNDPROC, (LONG_PTR)&AerodromeListPlugin::FilterProc);

    rows_ = CreateWindowExW(0, kRowsClass, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, hwnd_, nullptr,
                            inst, this);

    notifyHwnd_ = hwnd_;
    if (settings_.collapsed) SetCollapsed(true);
    Layout();
    RebuildList();
    if (indexReady_) ApplyIndex();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
}

void AerodromeListPlugin::Layout()
{
    if (!hwnd_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int W = rc.right, H = rc.bottom;

    headerRc_ = {1, 1, W - 1, 1 + kHeaderH};
    collapseRc_ = {W - 1 - 14, 3, W - 3, 1 + kHeaderH - 2};

    int y0 = headerRc_.bottom + 4;
    masterRc_ = {W - 5 - kMasterW, y0, W - 5, y0 + kToolbarH};
    fieldRc_ = {5, y0, masterRc_.left - 5, y0 + kToolbarH};
    rowsRc_ = {1, fieldRc_.bottom + 5, W - 1, H - 1};

    // Runway panel docked at the bottom; the list gets what is left (at least ~2 rows).
    panelRc_ = panelCloseRc_ = panelDialogRc_ = RECT{0, 0, 0, 0};
    int panelH = RunwayPanelHeight();
    if (panelH > 0 && !settings_.collapsed) {
        panelH = std::min(panelH, std::max(0, (int)(H - 1 - rowsRc_.top - 2 * kRowH - 3)));
        panelRc_ = {1, H - 1 - panelH, W - 1, H - 1};
        rowsRc_.bottom = panelRc_.top - 3;
        // 12x12 and 24x12 buttons: even sizes so the glyphs below centre exactly.
        int by = panelRc_.top + (kPanelHeaderH - 12) / 2;
        panelCloseRc_ = {W - 4 - 12, by, W - 4, by + 12};
        panelDialogRc_ = {panelCloseRc_.left - 4 - 24, by, panelCloseRc_.left - 4, by + 12};
    }

    bool show = !settings_.collapsed;
    ShowWindow(filter_, show ? SW_SHOW : SW_HIDE);
    ShowWindow(rows_, show ? SW_SHOW : SW_HIDE);
    if (!show) return;

    TEXTMETRICW tm{};
    HDC dc = GetDC(filter_);
    HGDIOBJ old = SelectObject(dc, listFont_);
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(filter_, dc);
    int editH = tm.tmHeight;
    int editY = fieldRc_.top + (kToolbarH - editH) / 2;
    MoveWindow(filter_, fieldRc_.left + 5, editY, fieldRc_.right - fieldRc_.left - 9, editH, TRUE);
    MoveWindow(rows_, rowsRc_.left, rowsRc_.top, rowsRc_.right - rowsRc_.left,
               std::max(0, (int)(rowsRc_.bottom - rowsRc_.top)), TRUE);
    ScrollTo(top_);
    TryRestoreScroll();
}

void AerodromeListPlugin::PaintFrame(HDC dc)
{
    RECT rc;
    GetClientRect(hwnd_, &rc);
    Fill(dc, rc, kBody);

    // outer edge
    HLine(dc, 0, rc.right, 0, kBevelMid);
    VLine(dc, 0, 0, rc.bottom, kBevelMid);
    HLine(dc, 0, rc.right, rc.bottom - 1, kBevelDark);
    VLine(dc, rc.right - 1, 0, rc.bottom, kBevelDark);

    // title strip
    Bevel(dc, headerRc_, kStrip, true);
    SelectObject(dc, titleFont_);
    RECT title = headerRc_;
    title.right = collapseRc_.left - 2;
    title.left += 4;
    std::wstring caption = L"Aerodromes";
    if (indexLoaded_ && index_.grpFolders.empty()) caption += L" - no ASR folder found";
    Text(dc, caption, title, kTextBright, DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);

    // collapse button
    bool down = pressed_ == Button::Collapse && hot_ == Button::Collapse;
    Bevel(dc, collapseRc_, kButton, !down);
    DrawTriangle(dc, (collapseRc_.left + collapseRc_.right) / 2, (collapseRc_.top + collapseRc_.bottom) / 2,
                 !settings_.collapsed, kTextBright);

    if (settings_.collapsed) return;

    // search field (sunken) and Master button (raised)
    Bevel(dc, fieldRc_, kField, false);
    down = pressed_ == Button::Master && hot_ == Button::Master;
    Bevel(dc, masterRc_, down ? kButtonDown : kButton, !down);
    SelectObject(dc, buttonFont_);
    RECT mr = masterRc_;
    if (down) OffsetRect(&mr, 1, 1);
    Text(dc, L"Master", mr, kTextBright, DT_CENTER | DT_VCENTER);

    // line above the list
    HLine(dc, 1, rc.right - 1, rowsRc_.top - 2, kBevelDark);
    HLine(dc, 1, rc.right - 1, rowsRc_.top - 1, kBevelMid);

    PaintRunwayPanel(dc);
}

AerodromeListPlugin::Button AerodromeListPlugin::ButtonAt(POINT pt) const
{
    if (PtInRect(&collapseRc_, pt)) return Button::Collapse;
    if (!settings_.collapsed && PtInRect(&masterRc_, pt)) return Button::Master;
    if (!settings_.collapsed && PtInRect(&panelCloseRc_, pt)) return Button::PanelClose;
    if (!settings_.collapsed && PtInRect(&panelDialogRc_, pt)) return Button::PanelDialog;
    return Button::None;
}

// --- runway panel ------------------------------------------------------------------

void AerodromeListPlugin::SetRunwayPanel(const std::string& icao)
{
    settings_.runwayPanel = icao;
    QueueSave();
    if (!hwnd_) return;
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
    InvalidateRows();
}

int AerodromeListPlugin::RunwayPanelHeight() const
{
    if (settings_.runwayPanel.empty()) return 0;
    const SectorAirport* a = SectorInfo(settings_.runwayPanel);
    if (!a) return 0;
    return kPanelHeaderH + (1 + (int)a->runways.size()) * kPanelRowH + 4;
}

bool AerodromeListPlugin::RunwayPanelHit(POINT pt, RunwayToggle& hit) const
{
    if (IsRectEmpty(&panelRc_) || !PtInRect(&panelRc_, pt)) return false;
    const SectorAirport* a = SectorInfo(settings_.runwayPanel);
    if (!a) return false;
    int y = pt.y - (panelRc_.top + kPanelHeaderH + 2);
    if (y < 0) return false;
    int row = y / kPanelRowH;
    if (row > (int)a->runways.size()) return false;
    int x = pt.x - panelRc_.left;
    if (x >= kChipDepX - 2 && x < kChipDepX + kChipW + 2) hit.dep = true;
    else if (x >= kChipArrX - 2 && x < kChipArrX + kChipW + 2) hit.dep = false;
    else return false;
    hit.row = row;
    return true;
}

void AerodromeListPlugin::PaintRunwayPanel(HDC dc)
{
    if (IsRectEmpty(&panelRc_)) return;
    const SectorAirport* a = SectorInfo(settings_.runwayPanel);
    if (!a) return;
    bool enabled = RunwayDialog::Available() && !runwayBusy_;

    RECT clip = panelRc_;
    HRGN rgn = CreateRectRgnIndirect(&clip);
    SelectClipRgn(dc, rgn);

    Fill(dc, panelRc_, kBody);
    RECT head{panelRc_.left, panelRc_.top, panelRc_.right, panelRc_.top + kPanelHeaderH};
    Bevel(dc, head, kStrip, true);
    SelectObject(dc, titleFont_);
    RECT tr = head;
    tr.left += 6;
    tr.right = panelDialogRc_.left - 4;
    std::wstring title = Widen(settings_.runwayPanel) + L"  active runways";
    Text(dc, title, tr, kTextBright, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

    // "..." opens EuroScope's own runway dialog, "x" closes the panel
    bool down = pressed_ == Button::PanelDialog && hot_ == Button::PanelDialog;
    Bevel(dc, panelDialogRc_, down ? kButtonDown : kButton, !down);
    {
        // three 2x2 dots, symmetric in the button (drawn, not text, so they sit in the middle)
        int o = down ? 1 : 0;
        int cx = (panelDialogRc_.left + panelDialogRc_.right) / 2 + o; // between cx-1 and cx
        int cy = (panelDialogRc_.top + panelDialogRc_.bottom) / 2 + o;
        for (int dx : {-5, -1, 3}) {
            RECT dot{cx + dx, cy - 1, cx + dx + 2, cy + 1};
            Fill(dc, dot, kTextBright);
        }
    }
    down = pressed_ == Button::PanelClose && hot_ == Button::PanelClose;
    Bevel(dc, panelCloseRc_, down ? kButtonDown : kButton, !down);
    {
        // 6x6 cross, symmetric in the 12x12 button
        int o = down ? 1 : 0;
        int l = panelCloseRc_.left + 3 + o, t = panelCloseRc_.top + 3 + o;
        HPEN pen = CreatePen(PS_SOLID, 1, kTextBright);
        HGDIOBJ old = SelectObject(dc, pen);
        MoveToEx(dc, l, t, nullptr);
        LineTo(dc, l + 6, t + 6);
        MoveToEx(dc, l + 5, t, nullptr);
        LineTo(dc, l - 1, t + 6);
        SelectObject(dc, old);
        DeleteObject(pen);
    }

    auto chip = [&](int x, int y, const wchar_t* label, bool on, COLORREF onColour) {
        RECT r{panelRc_.left + x, y, panelRc_.left + x + kChipW, y + kChipH};
        if (on) Bevel(dc, r, enabled ? onColour : kButton, true);
        else Bevel(dc, r, kField, false);
        Text(dc, label, r, on ? (enabled ? RGB(240, 240, 240) : kText) : kTextDim, DT_CENTER | DT_VCENTER);
    };

    SelectObject(dc, buttonFont_);
    int y = panelRc_.top + kPanelHeaderH + 2;
    for (int row = 0; row <= (int)a->runways.size(); ++row, y += kPanelRowH) {
        bool dep = row == 0 ? a->dep : a->runways[row - 1].dep;
        bool arr = row == 0 ? a->arr : a->runways[row - 1].arr;
        std::wstring label = row == 0 ? L"APT" : Widen(a->runways[row - 1].name);
        RECT lr{panelRc_.left + 8, y, panelRc_.left + kChipDepX - 4, y + kChipH};
        Text(dc, label, lr, row == 0 ? kTextDim : kTextBright, DT_LEFT | DT_VCENTER);
        int cy = y + (kPanelRowH - kChipH) / 2 - 1;
        chip(kChipDepX, cy, L"DEP", dep, kDepOn);
        chip(kChipArrX, cy, L"ARR", arr, kArrOn);
    }

    SelectClipRgn(dc, nullptr);
    DeleteObject(rgn);
}

bool AerodromeListPlugin::ApplyRunways(const std::string& icao, const RunwayDialog::AirportState& wanted)
{
    if (!RunwayDialog::Available()) {
        Message("Setting runways is unavailable in this EuroScope version (" + runwayDiagnostic_ +
                "). Use OTHER SET > Active airports/runways.");
        return false;
    }
    if (runwayBusy_) return false;
    runwayBusy_ = true;
    RunwayDialog::Result r = RunwayDialog::Apply(icao, wanted);
    runwayBusy_ = false;
    RefreshSectorAirports(true);
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    switch (r) {
    case RunwayDialog::Result::Applied:
        return true;
    case RunwayDialog::Result::AirportNotListed:
        Message(icao + " is not in EuroScope's active airport/runway list.");
        return false;
    default:
        Message("Could not set the runways of " + icao + " (" + runwayDiagnostic_ + ").");
        return false;
    }
}

void AerodromeListPlugin::ToggleRunway(const RunwayToggle& t)
{
    std::string icao = settings_.runwayPanel;
    const SectorAirport* a = SectorInfo(icao);
    if (!a || t.row < 0 || t.row > (int)a->runways.size()) return;

    RunwayDialog::AirportState want;
    want.airport.dep = a->dep;
    want.airport.arr = a->arr;
    for (const RunwayEnd& r : a->runways) want.runways[r.name] = RunwayDialog::Flags{r.dep, r.arr};

    if (t.row == 0) {
        bool& f = t.dep ? want.airport.dep : want.airport.arr;
        f = !f;
    } else {
        RunwayDialog::Flags& rf = want.runways[a->runways[t.row - 1].name];
        bool& f = t.dep ? rf.dep : rf.arr;
        f = !f;
        // a runway in use means the aerodrome is in use for that direction too
        if (f) (t.dep ? want.airport.dep : want.airport.arr) = true;
    }
    ApplyRunways(icao, want);
}

// spec: "27" (both ways), "27/14" (departure/arrival), "09,14/27", "27/-", or "OFF".
bool AerodromeListPlugin::SetActiveRunways(const std::string& icaoIn, const std::string& specIn)
{
    std::string icao = Upper(icaoIn);
    std::string spec = Upper(specIn);
    spec.erase(std::remove(spec.begin(), spec.end(), ' '), spec.end());
    const SectorAirport* a = SectorInfo(icao);
    if (!a) {
        Message(icao + " is not an aerodrome of the loaded sector file.");
        return false;
    }
    auto split = [](const std::string& s) {
        std::vector<std::string> out;
        size_t pos = 0;
        while (pos <= s.size()) {
            size_t c = s.find(',', pos);
            if (c == std::string::npos) c = s.size();
            std::string part = s.substr(pos, c - pos);
            if (!part.empty() && part != "-") out.push_back(part);
            pos = c + 1;
        }
        return out;
    };
    std::vector<std::string> dep, arr;
    if (spec != "OFF") {
        size_t slash = spec.find('/');
        dep = split(spec.substr(0, slash));
        arr = slash == std::string::npos ? dep : split(spec.substr(slash + 1));
        // Runway names in the sector file are zero-padded ("09"); accept "9" too.
        auto fix = [&](std::vector<std::string>& v) {
            for (std::string& n : v) {
                if (!n.empty() && isdigit((unsigned char)n[0]) && (n.size() == 1 || !isdigit((unsigned char)n[1])))
                    n = "0" + n;
                bool known = std::any_of(a->runways.begin(), a->runways.end(),
                                         [&](const RunwayEnd& r) { return r.name == n; });
                if (!known) {
                    std::string list;
                    for (auto& r : a->runways) list += (list.empty() ? "" : " ") + r.name;
                    Message(icao + " has no runway " + n + " (runways: " + list + ").");
                    return false;
                }
            }
            return true;
        };
        if (!fix(dep) || !fix(arr)) return false;
        if (dep.empty() && arr.empty()) {
            Message("Give a runway, e.g. " + icao + " 27 or " + icao + " 27/14.");
            return false;
        }
    }

    RunwayDialog::AirportState want;
    want.airport.dep = !dep.empty();
    want.airport.arr = !arr.empty();
    for (const RunwayEnd& r : a->runways) {
        RunwayDialog::Flags f;
        f.dep = std::find(dep.begin(), dep.end(), r.name) != dep.end();
        f.arr = std::find(arr.begin(), arr.end(), r.name) != arr.end();
        want.runways[r.name] = f;
    }
    if (!ApplyRunways(icao, want)) return false;
    SetRunwayPanel(icao);
    return true;
}

void AerodromeListPlugin::SetCollapsed(bool collapsed)
{
    RECT wr;
    GetWindowRect(hwnd_, &wr);
    if (collapsed && !settings_.collapsed) expandedHeight_ = wr.bottom - wr.top;
    settings_.collapsed = collapsed;
    int h = collapsed ? kHeaderH + 2 : std::max(expandedHeight_, 120);
    SetWindowPos(hwnd_, nullptr, 0, 0, wr.right - wr.left, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    Layout();
    InvalidateRect(hwnd_, nullptr, TRUE);
    SaveNow();
}

void AerodromeListPlugin::SetVisible(bool visible)
{
    if (!hwnd_) EnsureWindow();
    if (!hwnd_) return;
    SaveNow();
    ShowWindow(hwnd_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

void AerodromeListPlugin::SaveRect()
{
    if (!hwnd_) return;
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (!GetWindowPlacement(hwnd_, &wp)) return;
    settings_.rect = wp.rcNormalPosition;
    // remember the expanded size, also while collapsed
    if (settings_.collapsed) settings_.rect.bottom = settings_.rect.top + expandedHeight_;
    else expandedHeight_ = settings_.rect.bottom - settings_.rect.top;
}

void AerodromeListPlugin::QueueSave()
{
    // Restart a short timer on every change, so a burst of changes (dragging,
    // scrolling) is written once, a moment after it stops.
    if (hwnd_ && IsWindow(hwnd_)) SetTimer(hwnd_, kSaveTimer, kSaveDelayMs, nullptr);
    else SaveNow();
}

void AerodromeListPlugin::SaveNow()
{
    if (hwnd_ && IsWindow(hwnd_)) {
        KillTimer(hwnd_, kSaveTimer);
        SaveRect();
    }
    settings_.SaveWindow();
    settings_.SavePins();
}

LRESULT CALLBACK AerodromeListPlugin::FrameProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    AerodromeListPlugin* self;
    if (msg == WM_NCCREATE) {
        self = static_cast<AerodromeListPlugin*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        self->hwnd_ = h;
    } else {
        self = reinterpret_cast<AerodromeListPlugin*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(h, msg, wp, lp);
    return self->HandleFrame(msg, wp, lp);
}

LRESULT AerodromeListPlugin::HandleFrame(UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &pt);
        RECT rc;
        GetClientRect(hwnd_, &rc);
        bool l = pt.x < kGrip, r = pt.x >= rc.right - kGrip;
        bool t = pt.y < 3, b = !settings_.collapsed && pt.y >= rc.bottom - kGrip;
        if (!settings_.collapsed) {
            if (t && l) return HTTOPLEFT;
            if (t && r) return HTTOPRIGHT;
            if (b && l) return HTBOTTOMLEFT;
            if (b && r) return HTBOTTOMRIGHT;
            if (t) return HTTOP;
            if (b) return HTBOTTOM;
        }
        if (l) return HTLEFT;
        if (r) return HTRIGHT;
        if (PtInRect(&headerRc_, pt) && ButtonAt(pt) == Button::None) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_NCLBUTTONDBLCLK:
        if (wp == HTCAPTION) {
            SetCollapsed(!settings_.collapsed);
            return 0;
        }
        break;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize = {170, settings_.collapsed ? kHeaderH + 2 : 110};
        if (settings_.collapsed) mmi->ptMaxTrackSize.y = kHeaderH + 2;
        return 0;
    }
    case WM_SIZE:
        Layout();
        InvalidateRect(hwnd_, nullptr, FALSE);
        QueueSave();
        return 0;
    case WM_MOVE:
        QueueSave();
        return 0;
    case WM_EXITSIZEMOVE:
        SaveNow();
        return 0;
    case WM_TIMER:
        if (wp == kSaveTimer) {
            SaveNow();
            return 0;
        }
        break;
    case WM_DESTROY:
        // Last chance: EuroScope closing destroys this window (it is owned by
        // EuroScope's main window), possibly before the plug-in is unloaded.
        SaveNow();
        notifyHwnd_ = nullptr;
        return 0;
    case WM_NCDESTROY:
        hwnd_ = nullptr;
        filter_ = nullptr;
        rows_ = nullptr;
        return 0;
    case WM_CLOSE:
        return 0; // stays on screen (Alt+F4 etc.); .adl hides it for the session if really needed
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd_, &ps);
        RECT rc;
        GetClientRect(hwnd_, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);
        PaintFrame(mem);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, kTextBright);
        SetBkColor((HDC)wp, kField);
        return (LRESULT)fieldBrush_;
    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        pressed_ = hot_ = ButtonAt(pt);
        RunwayToggle hit;
        if (pressed_ != Button::None) {
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (PtInRect(&fieldRc_, pt)) {
            SetFocus(filter_);
        } else if (RunwayPanelHit(pt, hit)) {
            // DEP/ARR chip: apply after this click has been handled
            PostMessageW(hwnd_, WM_APP_RUNWAY, hit.row, hit.dep ? 1 : 0);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (pressed_ != Button::None) {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            Button h = ButtonAt(pt);
            if (h != hot_) {
                hot_ = h;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        return 0;
    case WM_LBUTTONUP: {
        Button b = pressed_;
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        pressed_ = hot_ = Button::None;
        if (GetCapture() == hwnd_) ReleaseCapture();
        InvalidateRect(hwnd_, nullptr, FALSE);
        if (b != Button::None && ButtonAt(pt) == b) {
            if (b == Button::Collapse) SetCollapsed(!settings_.collapsed);
            if (b == Button::Master) OpenMaster();
            if (b == Button::PanelClose) SetRunwayPanel("");
            if (b == Button::PanelDialog) {
                if (!RunwayDialog::Show(settings_.runwayPanel))
                    Message("EuroScope's runway dialog could not be opened from here (" + runwayDiagnostic_ +
                            "). Use OTHER SET > Active airports/runways.");
                RefreshSectorAirports(true);
            }
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (pressed_ != Button::None) {
            pressed_ = hot_ = Button::None;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (rows_) return SendMessageW(rows_, msg, wp, lp);
        return 0;
    case WM_SETFOCUS:
        if (rows_ && !settings_.collapsed) SetFocus(rows_);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_FILTER && HIWORD(wp) == EN_CHANGE) {
            top_ = 0;
            RebuildList();
            // A complete ICAO of the sector brings up its runways.
            wchar_t text[64] = {};
            GetWindowTextW(filter_, text, 64);
            std::string first = Upper(Narrow(text));
            while (!first.empty() && first.front() == ' ') first.erase(first.begin());
            first = first.substr(0, first.find(' '));
            if (first.size() == 4 && SectorInfo(first) && first != settings_.runwayPanel) SetRunwayPanel(first);
            return 0;
        }
        break;
    case WM_APP_RUNWAY: {
        HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        RunwayToggle t;
        t.row = (int)wp;
        t.dep = lp != 0;
        ToggleRunway(t);
        SetCursor(old);
        return 0;
    }
    case WM_APP_INDEX_READY:
        ApplyIndex();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_APP_OPEN_ITEM: {
        HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        OpenItem((int)wp, (int)lp);
        SetCursor(old);
        return 0;
    }
    case WM_APP_RETURN_FOCUS:
        // Give the keyboard back to EuroScope so the command line keeps working.
        if (owner_ && IsWindow(owner_)) SetActiveWindow(owner_);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

// --- window: rows ---------------------------------------------------------------

bool AerodromeListPlugin::HasScrollbar() const
{
    RECT rc;
    GetClientRect(rows_, &rc);
    return ContentHeight() > rc.bottom;
}

RECT AerodromeListPlugin::ThumbRect() const
{
    RECT rc;
    GetClientRect(rows_, &rc);
    int content = ContentHeight(), view = rc.bottom;
    if (content <= view || view <= 0) return RECT{0, 0, 0, 0};
    int thumbH = std::max(18, view * view / content);
    int range = content - view;
    int y = (int)((long long)top_ * (view - thumbH) / range);
    return RECT{rc.right - kScrollW + 1, y, rc.right - 1, y + thumbH};
}

int AerodromeListPlugin::ItemAt(int y) const
{
    if (y < 0) return -1;
    int i = (y + top_) / kRowH;
    return i < RowCount() ? i : -1;
}

void AerodromeListPlugin::ScrollTo(int top)
{
    if (!rows_) return;
    RECT rc;
    GetClientRect(rows_, &rc);
    int maxTop = std::max(0, ContentHeight() - (int)rc.bottom);
    top = std::max(0, std::min(top, maxTop));
    if (top != top_) {
        top_ = top;
        InvalidateRows();
    }
    // Remember where the full (unfiltered) list was scrolled to.
    if (scrollRestored_ && filter_ && GetWindowTextLengthW(filter_) == 0 && settings_.scrollTop != top_) {
        settings_.scrollTop = top_;
        QueueSave();
    }
}

// Put the list back where it was last time, once it actually has its rows
// (ASR index loaded, sector read) and its real size (not collapsed).
void AerodromeListPlugin::TryRestoreScroll()
{
    if (scrollRestored_ || !rows_ || !indexLoaded_ || RowCount() == 0 || settings_.collapsed) return;
    if (filter_ && GetWindowTextLengthW(filter_) > 0) return;
    scrollRestored_ = true;
    ScrollTo(settings_.scrollTop);
}

void AerodromeListPlugin::EnsureVisible(int item)
{
    if (item < 0) return;
    RECT rc;
    GetClientRect(rows_, &rc);
    int y = item * kRowH;
    if (y < top_) ScrollTo(y);
    else if (y + kRowH > top_ + rc.bottom) ScrollTo(y + kRowH - rc.bottom);
}

void AerodromeListPlugin::PaintRows(HDC dc, const RECT& rc)
{
    Fill(dc, rc, kBody);
    SelectObject(dc, listFont_);

    if (RowCount() == 0) {
        std::wstring msg;
        wchar_t text[8] = {};
        if (filter_) GetWindowTextW(filter_, text, 8);
        if (!indexLoaded_) msg = L"Loading...";
        else if (text[0]) msg = L"No match - Enter opens an exact ICAO";
        else msg = L"No aerodrome ASRs found";
        RECT r = rc;
        r.top += 8;
        Text(dc, msg, r, kTextDim, DT_CENTER | DT_TOP | DT_END_ELLIPSIS);
        return;
    }

    bool scroll = HasScrollbar();
    int right = rc.right - (scroll ? kScrollW + 2 : 4);
    SIZE icaoSize;
    GetTextExtentPoint32W(dc, L"WWWW", 4, &icaoSize);

    int first = top_ / kRowH;
    int last = std::min(RowCount() - 1, (int)((top_ + rc.bottom) / kRowH));
    for (int i = first; i <= last; ++i) {
        const Airport& a = airports_[visible_[i]];
        int y = i * kRowH - top_;
        RECT row{0, y, rc.right, y + kRowH};
        bool dragged = dragging_ && i == pressItem_;
        COLORREF bg = (i == sel_ || dragged) ? kSelected : (i == hover_ ? kHover : kBody);
        Fill(dc, row, bg);

        bool pinned = i < pinnedVisible_;
        DrawPin(dc, kPinW / 2, y + kRowH / 2, pinned, i == hover_ && hoverPin_);

        bool active = !lastOpenedIcao_.empty() && a.icao == lastOpenedIcao_;
        RECT ir{kPinW, y, kPinW + icaoSize.cx + 2, y + kRowH};
        Text(dc, Widen(a.icao), ir, active ? kActive : kTextBright, DT_LEFT | DT_VCENTER);

        // Active runways (click to open the runway panel for this aerodrome).
        RECT sr{right - kSummaryW, y, right, y + kRowH};
        if (a.icao == settings_.runwayPanel) {
            RECT cell{sr.left - 2, y + 1, sr.right + 2, y + kRowH - 1};
            Bevel(dc, cell, kField, false);
        }
        std::wstring summary = RunwaySummary(a.icao);
        if (!summary.empty()) Text(dc, summary, sr, kRunwayText, DT_RIGHT | DT_VCENTER | DT_END_ELLIPSIS);
        else if (i == hover_ && !hoverPin_) Text(dc, L"RWY", sr, kTextDim, DT_RIGHT | DT_VCENTER);
        // The aerodrome's name is shown on right-click (see ShowNameBox).
    }

    // separator between pinned and the rest
    if (pinnedVisible_ > 0 && pinnedVisible_ < RowCount()) {
        int y = pinnedVisible_ * kRowH - top_;
        HLine(dc, 0, rc.right, y - 1, kBevelMid);
    }

    // drop position while dragging
    if (dragging_ && dropSlot_ >= 0) {
        int y = dropSlot_ * kRowH - top_;
        RECT line{kPinW - 4, y - 1, right, y + 1};
        Fill(dc, line, kPinOn);
    }

    // scrollbar
    if (scroll) {
        RECT track{rc.right - kScrollW, 0, rc.right, rc.bottom};
        Fill(dc, track, kStrip);
        VLine(dc, track.left, 0, rc.bottom, kBevelDark);
        RECT thumb = ThumbRect();
        Bevel(dc, thumb, thumbDrag_ ? kBevelLight : kButton, true);
    }
}

// --- right-click: dark box with the aerodrome's name ------------------------------

void AerodromeListPlugin::ShowNameBox(int item, POINT screenPt)
{
    CloseNameBox();
    if (item < 0 || item >= RowCount()) return;
    // Copies: EuroScope's timer may rebuild the list while the box is open.
    const Airport& a = airports_[visible_[item]];
    box_.icao = a.icao;
    box_.name = a.name.empty() ? L"(no name in ICAO_Airports.txt)" : Widen(a.name);
    box_.asrs.clear();
    if (a.asrs.size() > 1) {
        for (const std::wstring& p : a.asrs) box_.asrs.push_back(p.substr(p.find_last_of(L'\\') + 1));
    }
    box_.hover = -1;

    // Size to the text.
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, listFont_);
    SIZE icao{}, name{}, space{};
    GetTextExtentPoint32W(dc, L"WWWW", 4, &icao);
    GetTextExtentPoint32W(dc, box_.name.c_str(), (int)box_.name.size(), &name);
    GetTextExtentPoint32W(dc, L"  ", 2, &space);
    int w = icao.cx + space.cx + name.cx;
    for (const std::wstring& f : box_.asrs) {
        SIZE s{};
        std::wstring label = L"Open " + f;
        GetTextExtentPoint32W(dc, label.c_str(), (int)label.size(), &s);
        w = std::max(w, (int)s.cx);
    }
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);

    const int pad = kBoxPad;
    int width = std::min(std::max(w + 2 * pad + 2, 120), 460);
    int height = 2 * pad + kBoxLineH + (box_.asrs.empty() ? 0 : 5 + (int)box_.asrs.size() * kBoxLineH) + 2;

    // Next to the pointer, kept on the monitor.
    HMONITOR mon = MonitorFromPoint(screenPt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    int x = std::min((int)screenPt.x + 6, (int)mi.rcWork.right - width);
    int y = std::min((int)screenPt.y + 6, (int)mi.rcWork.bottom - height);
    x = std::max(x, (int)mi.rcWork.left);
    y = std::max(y, (int)mi.rcWork.top);

    nameBox_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kBoxClass, L"", WS_POPUP, x, y,
                               width, height, hwnd_, nullptr, ThisModule(), this);
    if (!nameBox_) return;
    ShowWindow(nameBox_, SW_SHOWNOACTIVATE);
    SetCapture(nameBox_); // the next click anywhere closes it
}

void AerodromeListPlugin::CloseNameBox()
{
    if (nameBox_ && IsWindow(nameBox_)) DestroyWindow(nameBox_);
    nameBox_ = nullptr;
}

LRESULT CALLBACK AerodromeListPlugin::NameBoxProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    AerodromeListPlugin* self;
    if (msg == WM_NCCREATE) {
        self = static_cast<AerodromeListPlugin*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = reinterpret_cast<AerodromeListPlugin*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(h, msg, wp, lp);
    return self->HandleNameBox(h, msg, wp, lp);
}

LRESULT AerodromeListPlugin::HandleNameBox(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    RECT rc;
    GetClientRect(h, &rc);
    const int itemsTop = kBoxPad + kBoxLineH + 5;
    auto itemAt = [&](POINT pt) -> int {
        if (box_.asrs.empty() || pt.x < 0 || pt.x >= rc.right || pt.y < itemsTop) return -1;
        int i = (pt.y - itemsTop) / kBoxLineH;
        return i < (int)box_.asrs.size() ? i : -1;
    };

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);

        // EuroScope-style box: dark face, light/dark bevelled edge.
        Fill(mem, rc, kStrip);
        HLine(mem, 0, rc.right, 0, kBevelLight);
        VLine(mem, 0, 0, rc.bottom, kBevelLight);
        HLine(mem, 0, rc.right, rc.bottom - 1, kBevelDark);
        VLine(mem, rc.right - 1, 0, rc.bottom, kBevelDark);

        SelectObject(mem, listFont_);
        SIZE icao{};
        GetTextExtentPoint32W(mem, L"WWWW  ", 6, &icao);
        RECT line{kBoxPad, kBoxPad, rc.right - kBoxPad, kBoxPad + kBoxLineH};
        Text(mem, Widen(box_.icao), line, kTextBright, DT_LEFT | DT_VCENTER);
        RECT nameRc = line;
        nameRc.left += icao.cx;
        Text(mem, box_.name, nameRc, kText, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

        if (!box_.asrs.empty()) {
            HLine(mem, 4, rc.right - 4, itemsTop - 3, kBevelDark);
            HLine(mem, 4, rc.right - 4, itemsTop - 2, kBevelMid);
            for (int i = 0; i < (int)box_.asrs.size(); ++i) {
                RECT ir{2, itemsTop + i * kBoxLineH, rc.right - 2, itemsTop + (i + 1) * kBoxLineH};
                if (i == box_.hover) Fill(mem, ir, kSelected);
                ir.left = kBoxPad;
                Text(mem, L"Open " + box_.asrs[i], ir, kText, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
            }
        }

        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        // Moving well away from the box closes it, like a tooltip.
        if (pt.x < -60 || pt.y < -60 || pt.x > rc.right + 60 || pt.y > rc.bottom + 60) {
            PostMessageW(h, WM_CLOSE, 0, 0);
            return 0;
        }
        int i = itemAt(pt);
        if (i != box_.hover) {
            box_.hover = i;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int i = msg == WM_LBUTTONDOWN ? itemAt(pt) : -1;
        std::string icao = box_.icao;
        PostMessageW(h, WM_CLOSE, 0, 0);
        if (i >= 0) OpenIcao(icao, i);
        return 0;
    }
    case WM_CAPTURECHANGED:
        if ((HWND)lp != h) PostMessageW(h, WM_CLOSE, 0, 0);
        return 0;
    case WM_CLOSE:
        if (GetCapture() == h) ReleaseCapture();
        DestroyWindow(h);
        return 0;
    case WM_NCDESTROY:
        if (nameBox_ == h) nameBox_ = nullptr;
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK AerodromeListPlugin::RowsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    AerodromeListPlugin* self;
    if (msg == WM_NCCREATE) {
        self = static_cast<AerodromeListPlugin*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        self->rows_ = h;
    } else {
        self = reinterpret_cast<AerodromeListPlugin*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(h, msg, wp, lp);
    return self->HandleRows(msg, wp, lp);
}

LRESULT AerodromeListPlugin::HandleRows(UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(rows_, &ps);
        RECT rc;
        GetClientRect(rows_, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1, (int)rc.right), std::max(1, (int)rc.bottom));
        HGDIOBJ oldBmp = SelectObject(mem, bmp);
        PaintRows(mem, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(rows_, &ps);
        return 0;
    }
    case WM_SIZE:
        ScrollTo(top_);
        InvalidateRows();
        return 0;
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        ScrollTo(top_ - delta * kRowH * 3 / WHEEL_DELTA);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        SetFocus(rows_);
        RECT thumb = ThumbRect();
        RECT rc;
        GetClientRect(rows_, &rc);
        if (HasScrollbar() && pt.x >= rc.right - kScrollW) {
            if (PtInRect(&thumb, pt)) {
                thumbDrag_ = true;
                thumbGrab_ = pt.y - thumb.top;
                SetCapture(rows_);
            } else {
                ScrollTo(top_ + (pt.y < thumb.top ? -rc.bottom : rc.bottom)); // page
            }
            InvalidateRows();
            return 0;
        }
        int i = ItemAt(pt.y);
        if (i < 0) return 0;
        sel_ = i;
        if (pt.x < kPinW) {
            TogglePin(airports_[visible_[i]].icao);
            return 0;
        }
        // The runway cell at the right opens (or closes) the runway panel, not the ASR.
        int right = rc.right - (HasScrollbar() ? kScrollW + 2 : 4);
        if (pt.x >= right - kSummaryW - 2) {
            std::string icao = airports_[visible_[i]].icao;
            SetRunwayPanel(icao == settings_.runwayPanel ? "" : icao);
            return 0;
        }
        pressItem_ = i;
        pressPt_ = pt;
        dragging_ = false;
        dropSlot_ = -1;
        SetCapture(rows_);
        InvalidateRows();
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (!trackingMouse_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, rows_, 0};
            TrackMouseEvent(&tme);
            trackingMouse_ = true;
        }
        RECT rc;
        GetClientRect(rows_, &rc);
        if (thumbDrag_) {
            int content = ContentHeight(), view = rc.bottom;
            RECT thumb = ThumbRect();
            int thumbH = thumb.bottom - thumb.top;
            if (view > thumbH)
                ScrollTo((int)((long long)(pt.y - thumbGrab_) * (content - view) / (view - thumbH)));
            return 0;
        }
        if (pressItem_ >= 0 && GetCapture() == rows_) {
            // Pinned aerodromes can be dragged up and down to reorder them.
            if (!dragging_ && pressItem_ < pinnedVisible_ && abs(pt.y - pressPt_.y) > 4) dragging_ = true;
            if (dragging_) {
                if (pt.y < 0) ScrollTo(top_ - kRowH);
                if (pt.y > rc.bottom) ScrollTo(top_ + kRowH);
                int slot = (pt.y + top_ + kRowH / 2) / kRowH;
                dropSlot_ = std::max(0, std::min(slot, pinnedVisible_));
                SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
                InvalidateRows();
                return 0;
            }
        }
        int i = ItemAt(pt.y);
        bool onPin = i >= 0 && pt.x < kPinW;
        if (pt.x >= rc.right - (HasScrollbar() ? kScrollW : 0)) i = -1;
        if (i != hover_ || onPin != hoverPin_) {
            hover_ = i;
            hoverPin_ = onPin;
            InvalidateRows();
        }
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, dragging_ ? IDC_SIZENS : (hover_ >= 0 ? IDC_HAND : IDC_ARROW)));
            return TRUE;
        }
        break;
    case WM_MOUSELEAVE:
        trackingMouse_ = false;
        if (hover_ != -1) {
            hover_ = -1;
            hoverPin_ = false;
            InvalidateRows();
        }
        return 0;
    case WM_LBUTTONUP: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        bool wasThumb = thumbDrag_;
        bool wasDrag = dragging_;
        int pressed = pressItem_;
        int slot = dropSlot_;
        // Clear the state before releasing capture: ReleaseCapture() sends
        // WM_CAPTURECHANGED synchronously.
        thumbDrag_ = false;
        dragging_ = false;
        pressItem_ = -1;
        dropSlot_ = -1;
        if (GetCapture() == rows_) ReleaseCapture();
        if (wasThumb) {
            InvalidateRows();
            return 0;
        }
        // A pinned row released somewhere else counts as a drag, even if the
        // individual mouse moves were merged away.
        if (!wasDrag && pressed >= 0 && pressed < pinnedVisible_ && abs(pt.y - pressPt_.y) > 4) {
            slot = std::max(0, std::min((int)((pt.y + top_ + kRowH / 2) / kRowH), pinnedVisible_));
            wasDrag = true;
        }
        if (wasDrag) {
            ApplyDrop(pressed, slot);
        } else if (pressed >= 0 && ItemAt(pt.y) == pressed && pt.x >= kPinW) {
            PostMessageW(hwnd_, WM_APP_OPEN_ITEM, pressed, -1);
        }
        InvalidateRows();
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (thumbDrag_ || dragging_) {
            thumbDrag_ = false;
            dragging_ = false;
            pressItem_ = -1;
            dropSlot_ = -1;
            InvalidateRows();
        }
        return 0;
    case WM_RBUTTONUP: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int i = ItemAt(pt.y);
        if (i >= 0) {
            sel_ = i;
            InvalidateRows();
            ClientToScreen(rows_, &pt);
            ShowNameBox(i, pt);
        }
        return 0;
    }
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_KEYDOWN: {
        RECT rc;
        GetClientRect(rows_, &rc);
        int page = std::max(1, (int)rc.bottom / kRowH - 1);
        int n = RowCount();
        int next = sel_;
        switch (wp) {
        case VK_UP: next = sel_ <= 0 ? 0 : sel_ - 1; break;
        case VK_DOWN: next = sel_ < 0 ? 0 : std::min(n - 1, sel_ + 1); break;
        case VK_PRIOR: next = std::max(0, sel_ - page); break;
        case VK_NEXT: next = std::min(n - 1, std::max(0, sel_) + page); break;
        case VK_HOME: next = 0; break;
        case VK_END: next = n - 1; break;
        case VK_RETURN:
            if (sel_ >= 0) PostMessageW(hwnd_, WM_APP_OPEN_ITEM, sel_, -1);
            return 0;
        case VK_ESCAPE:
            PostMessageW(hwnd_, WM_APP_RETURN_FOCUS, 0, 0);
            return 0;
        default:
            return 0;
        }
        if (n > 0 && next != sel_) {
            sel_ = next;
            EnsureVisible(sel_);
            InvalidateRows();
        }
        return 0;
    }
    case WM_CHAR:
        // typing while the list has focus goes to the search field
        if (wp >= 0x20 && wp != 0x7F) {
            SetFocus(filter_);
            SendMessageW(filter_, EM_SETSEL, -1, -1);
            SendMessageW(filter_, WM_CHAR, wp, lp);
        }
        return 0;
    }
    return DefWindowProcW(rows_, msg, wp, lp);
}

// --- window: search field ---------------------------------------------------------

LRESULT CALLBACK AerodromeListPlugin::FilterProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* self = reinterpret_cast<AerodromeListPlugin*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self->HandleFilter(msg, wp, lp);
}

LRESULT AerodromeListPlugin::HandleFilter(UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_RETURN) {
            wchar_t text[64] = {};
            GetWindowTextW(filter_, text, 64);
            std::string typed = Upper(Narrow(text));
            while (!typed.empty() && typed.back() == ' ') typed.pop_back();
            while (!typed.empty() && typed.front() == ' ') typed.erase(typed.begin());

            // "VABB 27" / "VABB 27/14" / "VABB OFF" sets the active runways.
            size_t space = typed.find(' ');
            if (space != std::string::npos) {
                std::string icao = typed.substr(0, space);
                std::string spec = typed.substr(space + 1);
                if (SetActiveRunways(icao, spec)) {
                    SetWindowTextW(filter_, L"");
                    PostMessageW(hwnd_, WM_APP_RETURN_FOCUS, 0, 0);
                }
                return 0;
            }

            // A complete ICAO opens that aerodrome's ASR straight away.
            if (typed.size() == 4 && index_.asrByIcao.count(typed)) {
                SetWindowTextW(filter_, L"");
                OpenIcao(typed, -1);
                return 0;
            }
            // Otherwise open the highlighted entry, or the first match.
            int item = sel_ >= 0 ? sel_ : (RowCount() > 0 ? 0 : -1);
            if (item >= 0) {
                SetWindowTextW(filter_, L"");
                std::string icao = airports_[visible_[item]].icao;
                OpenIcao(icao, -1);
            } else if (!typed.empty()) {
                Message("No ASR found for " + typed + ".");
            }
            return 0;
        }
        if (wp == VK_DOWN && RowCount() > 0) {
            SetFocus(rows_);
            if (sel_ < 0) sel_ = 0;
            EnsureVisible(sel_);
            InvalidateRows();
            return 0;
        }
        if (wp == VK_ESCAPE) {
            SetWindowTextW(filter_, L"");
            PostMessageW(hwnd_, WM_APP_RETURN_FOCUS, 0, 0);
            return 0;
        }
        break;
    case WM_CHAR:
        if (wp == VK_RETURN || wp == VK_ESCAPE) return 0; // no beep
        break;
    case WM_MOUSEWHEEL:
        return SendMessageW(rows_, msg, wp, lp);
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(filter_, nullptr, TRUE);
        break;
    case WM_PAINT: {
        LRESULT r = CallWindowProcW(filterDefault_, filter_, msg, wp, lp);
        // placeholder text when empty and not being typed into
        if (GetWindowTextLengthW(filter_) == 0 && GetFocus() != filter_) {
            HDC dc = GetDC(filter_);
            RECT rc;
            GetClientRect(filter_, &rc);
            HGDIOBJ old = SelectObject(dc, listFont_);
            Text(dc, L"Search ICAO / name", rc, kTextDim, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
            SelectObject(dc, old);
            ReleaseDC(filter_, dc);
        }
        return r;
    }
    }
    return CallWindowProcW(filterDefault_, filter_, msg, wp, lp);
}

} // namespace

// ---------------------------------------------------------------------------
// EuroScope entry points
// ---------------------------------------------------------------------------

void __declspec(dllexport) EuroScopePlugInInit(EuroScopePlugIn::CPlugIn** ppPlugInInstance)
{
    g_plugin = new AerodromeListPlugin();
    *ppPlugInInstance = g_plugin;
}

void __declspec(dllexport) EuroScopePlugInExit(void)
{
    delete g_plugin;
    g_plugin = nullptr;
}
