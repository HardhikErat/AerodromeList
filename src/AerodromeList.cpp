// Aerodrome List - EuroScope plug-in
//
// Shows a small always-available window listing every aerodrome in the loaded
// sector file. Clicking an aerodrome opens its ground ASR (from
// <FIR>\ASR\GRP\*.asr), or switches to it if it is already open.

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#include <uxtheme.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../sdk/EuroScopePlugIn.h"
#include "AsrOpener.h"

namespace {

const char* const kPluginName = "Aerodrome List";
const char* const kPluginVersion = "1.0.0";
const char* const kPluginAuthor = "Hardhik Erat";
const char* const kPluginLicense = "Free to use";

const wchar_t* const kFirFolders[] = {L"VABF", L"VOMF", L"VIDF", L"VECF"};

const UINT WM_APP_INDEX_READY = WM_APP + 1;
const UINT WM_APP_OPEN_ITEM = WM_APP + 2;
const UINT WM_APP_RETURN_FOCUS = WM_APP + 3;

const int IDC_FILTER = 1001;
const int IDC_LIST = 1002;
const int IDM_ASR_BASE = 2000;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring Widen(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::string Upper(std::string s)
{
    for (char& c : s) c = (char)toupper((unsigned char)c);
    return s;
}

std::wstring UpperW(std::wstring s)
{
    CharUpperBuffW(&s[0], (DWORD)s.size());
    return s;
}

bool DirectoryExists(const std::wstring& path)
{
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring ParentDir(const std::wstring& path)
{
    size_t p = path.find_last_of(L"\\/");
    if (p == std::wstring::npos || p < 3) return {};
    return path.substr(0, p);
}

std::wstring ModulePath()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ModulePath), &self);
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(self, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    return std::wstring(buf, n);
}

std::wstring FullPath(const std::wstring& p)
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetFullPathNameW(p.c_str(), (DWORD)(sizeof(buf) / sizeof(buf[0])), buf, nullptr);
    return n ? std::wstring(buf, n) : p;
}

// "MUMBAI/CHHATRAPATI SHIVAJI" -> "Mumbai/Chhatrapati Shivaji"
std::string TitleCase(const std::string& s)
{
    std::string r = s;
    bool start = true;
    for (char& c : r) {
        unsigned char u = (unsigned char)c;
        if (isalpha(u)) {
            c = (char)(start ? toupper(u) : tolower(u));
            start = false;
        } else {
            start = (c != '\'');
        }
    }
    return r;
}

// ICAO code at the start of an ASR file name: "VABB-GND.asr", "VOAT GND.asr", "VIDP-SMR.asr".
std::string IcaoFromFileName(const std::wstring& file)
{
    std::string icao;
    for (wchar_t c : file) {
        if (icao.size() == 4) break;
        if (iswalnum(c)) {
            icao.push_back((char)towupper(c));
        } else {
            break;
        }
    }
    return icao.size() == 4 ? icao : std::string();
}

// ---------------------------------------------------------------------------
// ASR index (built on a worker thread)
// ---------------------------------------------------------------------------

struct AsrIndex {
    std::map<std::string, std::vector<std::wstring>> asrByIcao; // full paths, preferred first
    std::map<std::string, std::string> airportNames;
    std::vector<std::wstring> grpFolders;
};

void AddGrpFolder(const std::wstring& dir, std::vector<std::wstring>& out, std::set<std::wstring>& seen)
{
    if (!DirectoryExists(dir)) return;
    std::wstring key = UpperW(FullPath(dir));
    if (seen.insert(key).second) out.push_back(FullPath(dir));
}

void FindGrpFolders(const std::wstring& root, std::vector<std::wstring>& out, std::set<std::wstring>& seen)
{
    for (const wchar_t* fir : kFirFolders) {
        AddGrpFolder(root + L"\\" + fir + L"\\ASR\\GRP", out, seen);
    }
}

void LoadAirportNames(const std::wstring& file, std::map<std::string, std::string>& names)
{
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    std::string buf((size_t)size.QuadPart, '\0');
    DWORD read = 0;
    ReadFile(h, &buf[0], (DWORD)buf.size(), &read, nullptr);
    CloseHandle(h);
    buf.resize(read);

    size_t pos = 0;
    while (pos < buf.size()) {
        size_t eol = buf.find('\n', pos);
        if (eol == std::string::npos) eol = buf.size();
        std::string line = buf.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.empty() || line[0] == ';') continue;
        size_t t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        size_t t2 = line.find('\t', t1 + 1);
        std::string icao = Upper(line.substr(0, t1));
        std::string name = line.substr(t1 + 1, t2 == std::string::npos ? std::string::npos : t2 - t1 - 1);
        while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
        if (icao.size() == 4 && !name.empty() && !names.count(icao)) names[icao] = TitleCase(name);
    }
}

void PrewarmFile(const std::wstring& path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[64 * 1024];
    DWORD read = 0;
    while (ReadFile(h, buf, sizeof(buf), &read, nullptr) && read) {
    }
    CloseHandle(h);
}

// Directories of every DLL loaded into EuroScope (other plug-ins such as TopSky or
// GRplugin live inside <FIR>\Plugins, so they point straight at the sector tree).
std::vector<std::wstring> LoadedModuleDirs()
{
    std::vector<std::wstring> dirs;
    wchar_t winDir[MAX_PATH] = {};
    GetWindowsDirectoryW(winDir, MAX_PATH);
    std::wstring win = UpperW(winDir);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snap == INVALID_HANDLE_VALUE) return dirs;
    MODULEENTRY32W me{sizeof(me)};
    std::set<std::wstring> seen;
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        std::wstring dir = ParentDir(me.szExePath);
        std::wstring key = UpperW(dir);
        if (dir.empty() || key.compare(0, win.size(), win) == 0) continue;
        if (seen.insert(key).second) dirs.push_back(dir);
    }
    CloseHandle(snap);
    return dirs;
}

std::vector<std::wstring> SubDirectories(const std::wstring& dir)
{
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchLimitToDirectories, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (f == INVALID_HANDLE_VALUE) return out;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.' &&
            !(fd.dwFileAttributes & (FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_REPARSE_POINT))) {
            out.push_back(dir + L"\\" + fd.cFileName);
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return out;
}

AsrIndex BuildIndex(const std::wstring& pluginDir, const std::vector<std::wstring>& extraFolders)
{
    AsrIndex index;
    std::set<std::wstring> seen;

    // 1. Folders configured in the ini (either GRP folders or sector roots).
    for (const std::wstring& f : extraFolders) {
        AddGrpFolder(f, index.grpFolders, seen);
        FindGrpFolders(f, index.grpFolders, seen);
    }

    // 2. Walk up from a set of starting folders - this plug-in's folder, every
    //    other loaded plug-in's folder, and EuroScope's working folder - looking
    //    for <root>\<FIR>\ASR\GRP. Near the start folder we also look one and two
    //    levels into sibling folders, so separate VABF/VOMF/VIDF/VECF installs
    //    that sit side by side ("Sector Files\VABF-Install_...\VABF") are found
    //    whichever one the plug-in was loaded from.
    std::vector<std::wstring> starts;
    starts.push_back(pluginDir);
    for (const std::wstring& d : LoadedModuleDirs()) starts.push_back(d);
    wchar_t cwd[MAX_PATH * 2] = {};
    if (GetCurrentDirectoryW(MAX_PATH * 2, cwd)) starts.push_back(cwd);

    std::set<std::wstring> walked; // ancestor folders already searched
    for (const std::wstring& start : starts) {
        std::wstring dir = start;
        for (int depth = 0; depth < 5 && !dir.empty(); ++depth, dir = ParentDir(dir)) {
            if (!walked.insert(UpperW(dir)).second) continue;
            FindGrpFolders(dir, index.grpFolders, seen);
            if (depth == 0) continue;
            bool isTopLevel = ParentDir(dir).empty(); // e.g. "D:\VATSIM" - don't go two deep there
            for (const std::wstring& sub : SubDirectories(dir)) {
                FindGrpFolders(sub, index.grpFolders, seen);
                if (depth <= 3 && !isTopLevel) {
                    for (const std::wstring& sub2 : SubDirectories(sub)) FindGrpFolders(sub2, index.grpFolders, seen);
                }
            }
        }
    }

    // 3. Index *.asr files and pick up ICAO_Airports.txt next to them.
    bool namesLoaded = false;
    for (const std::wstring& grp : index.grpFolders) {
        std::vector<std::wstring> files;
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileExW((grp + L"\\*.asr").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (f != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(fd.cFileName);
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
        std::sort(files.begin(), files.end(),
                  [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
        for (const std::wstring& file : files) {
            std::string icao = IcaoFromFileName(file);
            if (!icao.empty()) index.asrByIcao[icao].push_back(grp + L"\\" + file);
        }

        if (!namesLoaded) {
            std::wstring firDir = ParentDir(ParentDir(grp)); // ...\<FIR>
            std::wstring namesFile = firDir + L"\\ICAO\\ICAO_Airports.txt";
            if (GetFileAttributesW(namesFile.c_str()) != INVALID_FILE_ATTRIBUTES) {
                LoadAirportNames(namesFile, index.airportNames);
                namesLoaded = true;
            }
        }
    }

    return index;
}

// ---------------------------------------------------------------------------
// Settings (AerodromeList.ini next to the DLL)
// ---------------------------------------------------------------------------

struct Settings {
    std::wstring file;
    RECT rect{0, 0, 0, 0};
    bool visible = true;
    bool topmost = false;
    std::vector<std::wstring> extraFolders;

    void Load()
    {
        wchar_t buf[2048];
        rect.left = GetPrivateProfileIntW(L"Window", L"X", INT_MIN, file.c_str());
        rect.top = GetPrivateProfileIntW(L"Window", L"Y", INT_MIN, file.c_str());
        int w = GetPrivateProfileIntW(L"Window", L"Width", 0, file.c_str());
        int h = GetPrivateProfileIntW(L"Window", L"Height", 0, file.c_str());
        rect.right = rect.left + w;
        rect.bottom = rect.top + h;
        visible = true; // the window always comes up at start-up; .adl hides it for the session only
        topmost = GetPrivateProfileIntW(L"Window", L"AlwaysOnTop", 0, file.c_str()) != 0;
        GetPrivateProfileStringW(L"Folders", L"AsrFolders", L"", buf, 2048, file.c_str());
        std::wstring all = buf;
        size_t pos = 0;
        while (pos <= all.size()) {
            size_t sep = all.find(L';', pos);
            if (sep == std::wstring::npos) sep = all.size();
            std::wstring part = all.substr(pos, sep - pos);
            while (!part.empty() && iswspace(part.back())) part.pop_back();
            while (!part.empty() && iswspace(part.front())) part.erase(part.begin());
            if (!part.empty()) {
                if (part.size() > 1 && part[1] != L':' && part[0] != L'\\') part = ParentDir(file) + L"\\" + part;
                extraFolders.push_back(part);
            }
            pos = sep + 1;
        }
    }

    void Save() const
    {
        auto put = [&](const wchar_t* key, int v) {
            WritePrivateProfileStringW(L"Window", key, std::to_wstring(v).c_str(), file.c_str());
        };
        put(L"X", rect.left);
        put(L"Y", rect.top);
        put(L"Width", rect.right - rect.left);
        put(L"Height", rect.bottom - rect.top);
        WritePrivateProfileStringW(L"Window", L"Visible", nullptr, file.c_str()); // no longer persisted
        put(L"AlwaysOnTop", topmost ? 1 : 0);
    }

    bool HasRect() const
    {
        return rect.left != INT_MIN && rect.top != INT_MIN && rect.right - rect.left > 50 &&
               rect.bottom - rect.top > 80;
    }
};

// ---------------------------------------------------------------------------
// Colours
// ---------------------------------------------------------------------------

const COLORREF kBack = RGB(0x1C, 0x20, 0x24);
const COLORREF kBackAlt = RGB(0x22, 0x27, 0x2C);
const COLORREF kEditBack = RGB(0x2A, 0x30, 0x36);
const COLORREF kText = RGB(0xE2, 0xE6, 0xEA);
const COLORREF kTextDim = RGB(0x8A, 0x93, 0x9C);
const COLORREF kTextNoAsr = RGB(0x5C, 0x63, 0x6A);
const COLORREF kHover = RGB(0x2C, 0x3A, 0x48);
const COLORREF kSelected = RGB(0x1F, 0x5A, 0x8C);
const COLORREF kAccent = RGB(0x3D, 0xA5, 0xFF);
const COLORREF kActive = RGB(0x47, 0xC2, 0x7A);

// ---------------------------------------------------------------------------
// Plug-in
// ---------------------------------------------------------------------------

struct Airport {
    std::string icao;
    std::string name;
    std::vector<std::wstring> asrs;
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
    // window plumbing
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK ListProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK FilterProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT, WPARAM, LPARAM);
    LRESULT HandleListMessage(UINT, WPARAM, LPARAM);
    LRESULT HandleFilterMessage(UINT, WPARAM, LPARAM);

    void EnsureWindow();
    void CreateFonts();
    void Layout();
    void DrawItem(const DRAWITEMSTRUCT* dis);
    void ShowAsrMenu(int item, POINT screenPt);
    void SetVisible(bool visible);
    void SaveRect();

    // data
    void StartIndexing();
    void ApplyIndex();
    bool RefreshSectorAirports(bool force);
    void RebuildList();
    void OpenItem(int item, int asrChoice);
    void Message(const std::string& text, bool flash = false);

    HWND FindEuroScopeWindow() const;

    HWND hwnd_ = nullptr;
    HWND filter_ = nullptr;
    HWND list_ = nullptr;
    HWND owner_ = nullptr;
    WNDPROC listDefault_ = nullptr;
    WNDPROC filterDefault_ = nullptr;
    HFONT font_ = nullptr;
    HFONT fontBold_ = nullptr;
    HBRUSH backBrush_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    int itemHeight_ = 20;
    int hover_ = -1;
    bool trackingMouse_ = false;
    int pressedItem_ = -1;

    Settings settings_;
    std::wstring pluginDir_;

    std::vector<std::string> sectorAirports_; // ICAO codes, sorted
    std::string sectorSignature_;
    std::vector<Airport> airports_;           // full list
    std::vector<int> visible_;                // indices into airports_ after filtering
    std::string lastOpenedIcao_;

    std::thread indexThread_;
    std::mutex indexMutex_;
    std::unique_ptr<AsrIndex> pendingIndex_;
    std::atomic<bool> indexReady_{false};
    std::atomic<HWND> notifyHwnd_{nullptr};
    AsrIndex index_;
    bool indexLoaded_ = false;
    int reindexAttempts_ = 0;

    bool openerReady_ = false;
    std::string openerDiagnostic_;
};

AerodromeListPlugin* g_plugin = nullptr;
const wchar_t* const kWindowClass = L"AerodromeListPluginWindow";

AerodromeListPlugin::AerodromeListPlugin()
    : CPlugIn(EuroScopePlugIn::COMPATIBILITY_CODE, kPluginName, kPluginVersion, kPluginAuthor, kPluginLicense)
{
    std::wstring dll = ModulePath();
    pluginDir_ = ParentDir(dll);
    settings_.file = pluginDir_ + L"\\AerodromeList.ini";
    settings_.Load();

    openerReady_ = AsrOpener::Initialise(openerDiagnostic_);

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
    if (hwnd_) {
        SaveRect();
        settings_.Save();
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    // The class points at code in this DLL; unregister it so a reload re-registers cleanly.
    HINSTANCE inst = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&AerodromeListPlugin::WndProc), &inst);
    UnregisterClassW(kWindowClass, inst);
    if (font_) DeleteObject(font_);
    if (fontBold_) DeleteObject(fontBold_);
    if (backBrush_) DeleteObject(backBrush_);
    if (editBrush_) DeleteObject(editBrush_);
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
    std::vector<std::string> codes;
    for (EuroScopePlugIn::CSectorElement e = SectorFileElementSelectFirst(EuroScopePlugIn::SECTOR_ELEMENT_AIRPORT);
         e.IsValid(); e = SectorFileElementSelectNext(e, EuroScopePlugIn::SECTOR_ELEMENT_AIRPORT)) {
        const char* n = e.GetName();
        if (!n) continue;
        std::string code = Upper(n);
        while (!code.empty() && isspace((unsigned char)code.back())) code.pop_back();
        if (!code.empty()) codes.push_back(code);
    }
    std::sort(codes.begin(), codes.end());
    codes.erase(std::unique(codes.begin(), codes.end()), codes.end());

    std::string sig;
    for (auto& c : codes) sig += c + ",";
    if (!force && sig == sectorSignature_) return false;
    sectorSignature_ = sig;
    sectorAirports_ = std::move(codes);
    RebuildList();
    return true;
}

void AerodromeListPlugin::RebuildList()
{
    airports_.clear();
    airports_.reserve(sectorAirports_.size());
    for (const std::string& icao : sectorAirports_) {
        Airport a;
        a.icao = icao;
        auto n = index_.airportNames.find(icao);
        if (n != index_.airportNames.end()) a.name = n->second;
        auto f = index_.asrByIcao.find(icao);
        if (f == index_.asrByIcao.end() || f->second.empty()) continue; // only aerodromes that have an ASR
        a.asrs = f->second;
        airports_.push_back(std::move(a));
    }

    if (!list_) return;

    // Filter
    wchar_t text[64] = {};
    if (filter_) GetWindowTextW(filter_, text, 64);
    std::string needle = Upper(Narrow(text));
    while (!needle.empty() && needle.back() == ' ') needle.pop_back();

    std::wstring selectedIcao;
    int sel = (int)SendMessageW(list_, LB_GETCURSEL, 0, 0);
    if (sel >= 0) {
        wchar_t buf[16] = {};
        if (SendMessageW(list_, LB_GETTEXTLEN, sel, 0) < 15) SendMessageW(list_, LB_GETTEXT, sel, (LPARAM)buf);
        selectedIcao = buf;
    }

    visible_.clear();
    for (int i = 0; i < (int)airports_.size(); ++i) {
        const Airport& a = airports_[i];
        if (!needle.empty() && a.icao.find(needle) == std::string::npos &&
            Upper(a.name).find(needle) == std::string::npos) {
            continue;
        }
        visible_.push_back(i);
    }

    SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(list_, LB_RESETCONTENT, 0, 0);
    for (int idx : visible_) {
        SendMessageW(list_, LB_ADDSTRING, 0, (LPARAM)Widen(airports_[idx].icao).c_str());
    }
    if (!selectedIcao.empty()) {
        LRESULT found = SendMessageW(list_, LB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)selectedIcao.c_str());
        if (found != LB_ERR) SendMessageW(list_, LB_SETCURSEL, found, 0);
    }
    hover_ = -1;
    SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list_, nullptr, TRUE);

    wchar_t title[96];
    if (indexLoaded_ && index_.grpFolders.empty())
        swprintf_s(title, L"Aerodromes  (no ASR folder found)");
    else
        swprintf_s(title, L"Aerodromes  %d", (int)airports_.size());
    SetWindowTextW(hwnd_, title);
}

void AerodromeListPlugin::OpenItem(int item, int asrChoice)
{
    if (item < 0 || item >= (int)visible_.size()) return;
    const Airport& a = airports_[visible_[item]];
    if (a.asrs.empty()) {
        Message("No ASR found for " + a.icao + " in the GRP folders.");
        return;
    }
    int choice = (asrChoice >= 0 && asrChoice < (int)a.asrs.size()) ? asrChoice : 0;
    std::string path = Narrow(a.asrs[choice]);

    AsrOpener::Result r = AsrOpener::Open(path);
    if (r == AsrOpener::Result::Opened) {
        lastOpenedIcao_ = a.icao;
        InvalidateRect(list_, nullptr, FALSE);
        PostMessageW(hwnd_, WM_APP_RETURN_FOCUS, 0, 0);
        return;
    }

    // Fallback: put the path on the clipboard so it can be pasted in Open SCT > Open...
    if (OpenClipboard(hwnd_)) {
        EmptyClipboard();
        std::wstring w = a.asrs[choice];
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
        if (mem) {
            memcpy(GlobalLock(mem), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(mem);
            SetClipboardData(CF_UNICODETEXT, mem);
        }
        CloseClipboard();
    }
    if (r == AsrOpener::Result::Refused) {
        Message("EuroScope could not open " + path + " (path copied to clipboard).");
    } else if (r == AsrOpener::Result::Crashed) {
        Message("Opening the ASR failed inside EuroScope; direct loading is now disabled for this session. "
                "Path copied to clipboard: " + path);
    } else {
        Message("Direct ASR loading is unavailable in this EuroScope version. Path copied to clipboard: " + path);
    }
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
        settings_.Save();
        Message(settings_.topmost ? "Window is now always on top of all apps." : "Window stays above EuroScope only.");
    } else if (arg == "INFO") {
        std::string folders;
        for (auto& f : index_.grpFolders) folders += (folders.empty() ? "" : " | ") + Narrow(f);
        Message("ASR loader: " + openerDiagnostic_ + ". GRP folders: " + (folders.empty() ? "none" : folders));
    } else {
        SetVisible(!(hwnd_ && IsWindowVisible(hwnd_)));
    }
    return true;
}

// --- window -------------------------------------------------------------------

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

void AerodromeListPlugin::CreateFonts()
{
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = -12;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    font_ = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_SEMIBOLD;
    lf.lfHeight = -13;
    wcscpy_s(lf.lfFaceName, L"Consolas");
    fontBold_ = CreateFontIndirectW(&lf);
}

void AerodromeListPlugin::EnsureWindow()
{
    if (hwnd_) return;

    HINSTANCE inst = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&AerodromeListPlugin::WndProc), &inst);

    WNDCLASSEXW wc{sizeof(wc)};
    if (!GetClassInfoExW(inst, kWindowClass, &wc)) {
        wc = WNDCLASSEXW{sizeof(wc)};
        wc.lpfnWndProc = &AerodromeListPlugin::WndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kWindowClass;
        wc.style = CS_DBLCLKS;
        RegisterClassExW(&wc);
    }

    backBrush_ = CreateSolidBrush(kBack);
    editBrush_ = CreateSolidBrush(kEditBack);
    CreateFonts();

    owner_ = FindEuroScopeWindow();

    RECT r = settings_.rect;
    // Ignore a saved position that is no longer on any monitor (e.g. a display was removed).
    bool onScreen = settings_.HasRect() && MonitorFromRect(&r, MONITOR_DEFAULTTONULL) != nullptr;
    if (!onScreen) {
        RECT work;
        HMONITOR mon = MonitorFromWindow(owner_ ? owner_ : GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(mon, &mi);
        work = mi.rcWork;
        int w = 230, h = 380;
        r = {work.right - w - 24, work.top + 140, work.right - 24, work.top + 140 + h};
    }

    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | (settings_.topmost ? WS_EX_TOPMOST : 0), kWindowClass, L"Aerodromes",
                            WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_CLIPCHILDREN, r.left, r.top,
                            r.right - r.left, r.bottom - r.top, owner_, nullptr, inst, this);
    if (!hwnd_) return;

    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));

    filter_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_UPPERCASE, 0, 0, 10, 10,
                              hwnd_, (HMENU)(INT_PTR)IDC_FILTER, inst, nullptr);
    SendMessageW(filter_, WM_SETFONT, (WPARAM)font_, TRUE);
    SendMessageW(filter_, EM_SETCUEBANNER, TRUE, (LPARAM)L"Filter (ICAO or name)");
    SendMessageW(filter_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(6, 6));
    SetWindowLongPtrW(filter_, GWLP_USERDATA, (LONG_PTR)this);
    filterDefault_ = (WNDPROC)SetWindowLongPtrW(filter_, GWLP_WNDPROC, (LONG_PTR)&AerodromeListPlugin::FilterProc);

    list_ = CreateWindowExW(0, L"LISTBOX", L"",
                            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS |
                                LBS_NOINTEGRALHEIGHT | LBS_WANTKEYBOARDINPUT,
                            0, 0, 10, 10, hwnd_, (HMENU)(INT_PTR)IDC_LIST, inst, nullptr);
    SendMessageW(list_, WM_SETFONT, (WPARAM)font_, TRUE);
    SendMessageW(list_, LB_SETITEMHEIGHT, 0, itemHeight_);
    SetWindowTheme(list_, L"DarkMode_Explorer", nullptr);
    SetWindowLongPtrW(list_, GWLP_USERDATA, (LONG_PTR)this);
    listDefault_ = (WNDPROC)SetWindowLongPtrW(list_, GWLP_WNDPROC, (LONG_PTR)&AerodromeListPlugin::ListProc);

    notifyHwnd_ = hwnd_;
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
    const int pad = 6, editH = 24;
    MoveWindow(filter_, pad, pad, rc.right - 2 * pad, editH, TRUE);
    MoveWindow(list_, 0, pad * 2 + editH, rc.right, rc.bottom - (pad * 2 + editH), TRUE);
}

void AerodromeListPlugin::SetVisible(bool visible)
{
    if (!hwnd_) EnsureWindow();
    if (!hwnd_) return;
    settings_.visible = visible;
    SaveRect();
    ShowWindow(hwnd_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    settings_.Save();
}

void AerodromeListPlugin::SaveRect()
{
    if (!hwnd_) return;
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (GetWindowPlacement(hwnd_, &wp)) settings_.rect = wp.rcNormalPosition;
}

void AerodromeListPlugin::DrawItem(const DRAWITEMSTRUCT* dis)
{
    if (dis->itemID == (UINT)-1) return;
    int item = (int)dis->itemID;
    if (item >= (int)visible_.size()) return;
    const Airport& a = airports_[visible_[item]];
    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;

    bool selected = (dis->itemState & ODS_SELECTED) != 0;
    bool hovered = item == hover_;
    bool hasAsr = !a.asrs.empty();
    bool active = !lastOpenedIcao_.empty() && a.icao == lastOpenedIcao_;

    COLORREF bg = selected ? kSelected : hovered && hasAsr ? kHover : (item % 2 ? kBackAlt : kBack);
    HBRUSH b = CreateSolidBrush(bg);
    FillRect(dc, &rc, b);
    DeleteObject(b);

    if (active) {
        RECT bar = {rc.left, rc.top + 3, rc.left + 3, rc.bottom - 3};
        HBRUSH ab = CreateSolidBrush(kActive);
        FillRect(dc, &bar, ab);
        DeleteObject(ab);
    }

    SetBkMode(dc, TRANSPARENT);
    RECT tr = rc;
    tr.left += 10;

    HGDIOBJ old = SelectObject(dc, fontBold_);
    SetTextColor(dc, hasAsr ? (selected ? RGB(255, 255, 255) : kText) : kTextNoAsr);
    std::wstring icao = Widen(a.icao);
    DrawTextW(dc, icao.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

    SIZE sz;
    GetTextExtentPoint32W(dc, L"WWWW", 4, &sz);
    tr.left += sz.cx + 10;

    if (a.asrs.size() > 1) {
        // little "+n" badge for aerodromes with more than one ASR (right-click to choose)
        SelectObject(dc, font_);
        wchar_t badge[8];
        swprintf_s(badge, L"+%d", (int)a.asrs.size() - 1);
        RECT br = tr;
        br.right -= 6;
        SetTextColor(dc, kAccent);
        DrawTextW(dc, badge, -1, &br, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
        SIZE bsz;
        GetTextExtentPoint32W(dc, badge, (int)wcslen(badge), &bsz);
        tr.right -= bsz.cx + 12;
    }

    SelectObject(dc, font_);
    SetTextColor(dc, hasAsr ? (selected ? RGB(220, 232, 245) : kTextDim) : kTextNoAsr);
    std::wstring name = a.name.empty() ? (hasAsr ? L"" : L"no ASR") : Widen(a.name);
    tr.right -= 6;
    DrawTextW(dc, name.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, old);
}

void AerodromeListPlugin::ShowAsrMenu(int item, POINT screenPt)
{
    if (item < 0 || item >= (int)visible_.size()) return;
    const Airport& a = airports_[visible_[item]];
    HMENU menu = CreatePopupMenu();
    if (a.asrs.empty()) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"No ASR files found");
    } else {
        for (size_t i = 0; i < a.asrs.size(); ++i) {
            const std::wstring& p = a.asrs[i];
            std::wstring label = p.substr(p.find_last_of(L'\\') + 1);
            AppendMenuW(menu, MF_STRING, IDM_ASR_BASE + i, label.c_str());
        }
    }
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, screenPt.x, screenPt.y, 0, hwnd_,
                             nullptr);
    DestroyMenu(menu);
    if (cmd >= IDM_ASR_BASE) OpenItem(item, cmd - IDM_ASR_BASE);
}

LRESULT CALLBACK AerodromeListPlugin::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
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
    return self->HandleMessage(msg, wp, lp);
}

LRESULT AerodromeListPlugin::HandleMessage(UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        Layout();
        return 0;
    case WM_EXITSIZEMOVE:
        SaveRect();
        settings_.Save();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize = {170, 140};
        return 0;
    }
    case WM_CLOSE:
        return 0; // stays on screen (Alt+F4 etc.); .adl hides it for the session if really needed
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        FillRect((HDC)wp, &rc, backBrush_);
        return 1;
    }
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, kText);
        SetBkColor((HDC)wp, kEditBack);
        return (LRESULT)editBrush_;
    case WM_CTLCOLORLISTBOX:
        return (LRESULT)backBrush_;
    case WM_MEASUREITEM:
        reinterpret_cast<MEASUREITEMSTRUCT*>(lp)->itemHeight = itemHeight_;
        return TRUE;
    case WM_DRAWITEM:
        DrawItem(reinterpret_cast<DRAWITEMSTRUCT*>(lp));
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_FILTER && HIWORD(wp) == EN_CHANGE) {
            RebuildList();
            return 0;
        }
        break;
    case WM_APP_INDEX_READY:
        ApplyIndex();
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

LRESULT CALLBACK AerodromeListPlugin::ListProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* self = reinterpret_cast<AerodromeListPlugin*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self->HandleListMessage(msg, wp, lp);
}

LRESULT AerodromeListPlugin::HandleListMessage(UINT msg, WPARAM wp, LPARAM lp)
{
    auto itemAt = [&](LPARAM pt) -> int {
        LRESULT r = CallWindowProcW(listDefault_, list_, LB_ITEMFROMPOINT, 0, pt);
        if (HIWORD(r)) return -1;
        return (int)LOWORD(r);
    };
    auto invalidateItem = [&](int i) {
        RECT r;
        if (i >= 0 && CallWindowProcW(listDefault_, list_, LB_GETITEMRECT, i, (LPARAM)&r) != LB_ERR)
            InvalidateRect(list_, &r, FALSE);
    };

    switch (msg) {
    case WM_MOUSEMOVE: {
        if (!trackingMouse_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, list_, 0};
            TrackMouseEvent(&tme);
            trackingMouse_ = true;
        }
        int i = itemAt(lp);
        if (i != hover_) {
            int old = hover_;
            hover_ = i;
            invalidateItem(old);
            invalidateItem(i);
            bool clickable = i >= 0 && i < (int)visible_.size() && !airports_[visible_[i]].asrs.empty();
            SetCursor(LoadCursorW(nullptr, clickable ? IDC_HAND : IDC_ARROW));
        }
        break;
    }
    case WM_SETCURSOR: {
        if (LOWORD(lp) != HTCLIENT) break;
        bool clickable = hover_ >= 0 && hover_ < (int)visible_.size() && !airports_[visible_[hover_]].asrs.empty();
        SetCursor(LoadCursorW(nullptr, clickable ? IDC_HAND : IDC_ARROW));
        return TRUE;
    }
    case WM_MOUSELEAVE: {
        trackingMouse_ = false;
        int old = hover_;
        hover_ = -1;
        invalidateItem(old);
        break;
    }
    case WM_LBUTTONDOWN:
        pressedItem_ = itemAt(lp);
        break;
    case WM_LBUTTONUP: {
        LRESULT r = CallWindowProcW(listDefault_, list_, msg, wp, lp);
        int i = itemAt(lp);
        if (i >= 0 && i == pressedItem_) PostMessageW(hwnd_, WM_APP_OPEN_ITEM, i, -1);
        pressedItem_ = -1;
        return r;
    }
    case WM_LBUTTONDBLCLK:
        return 0; // a single click already opens; ignore the second click
    case WM_RBUTTONUP: {
        int i = itemAt(lp);
        if (i >= 0) {
            CallWindowProcW(listDefault_, list_, LB_SETCURSEL, i, 0);
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(list_, &pt);
            ShowAsrMenu(i, pt);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_RETURN) {
            int sel = (int)CallWindowProcW(listDefault_, list_, LB_GETCURSEL, 0, 0);
            if (sel >= 0) PostMessageW(hwnd_, WM_APP_OPEN_ITEM, sel, -1);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            PostMessageW(hwnd_, WM_APP_RETURN_FOCUS, 0, 0);
            return 0;
        }
        break;
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    }
    return CallWindowProcW(listDefault_, list_, msg, wp, lp);
}

LRESULT CALLBACK AerodromeListPlugin::FilterProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* self = reinterpret_cast<AerodromeListPlugin*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self->HandleFilterMessage(msg, wp, lp);
}

LRESULT AerodromeListPlugin::HandleFilterMessage(UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN) {
        if (wp == VK_RETURN) {
            // open the selected entry, or the first one that has an ASR
            int sel = (int)SendMessageW(list_, LB_GETCURSEL, 0, 0);
            if (sel < 0) {
                for (int i = 0; i < (int)visible_.size(); ++i) {
                    if (!airports_[visible_[i]].asrs.empty()) {
                        sel = i;
                        break;
                    }
                }
            }
            if (sel >= 0) PostMessageW(hwnd_, WM_APP_OPEN_ITEM, sel, -1);
            return 0;
        }
        if (wp == VK_DOWN && !visible_.empty()) {
            SetFocus(list_);
            if (SendMessageW(list_, LB_GETCURSEL, 0, 0) < 0) SendMessageW(list_, LB_SETCURSEL, 0, 0);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            SetWindowTextW(filter_, L"");
            PostMessageW(hwnd_, WM_APP_RETURN_FOCUS, 0, 0);
            return 0;
        }
    }
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE)) return 0; // no beep
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
