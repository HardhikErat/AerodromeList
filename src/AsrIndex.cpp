// ASR index: finds <FIR>\ASR\GRP folders and the ASR files in them.
#include "AsrIndex.h"

#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <set>

namespace adl {

const wchar_t* const kFirFolders[] = {L"VABF", L"VOMF", L"VIDF", L"VECF"};

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

// Adds a <FIR>\ASR\GRP folder. Only the first folder found for each FIR is kept:
// folders are found closest-to-EuroScope's-plug-ins first, so older copies of
// the same FIR's sector files elsewhere on disk are ignored.
void AddGrpFolder(const std::wstring& dir, std::vector<std::wstring>& out, std::set<std::wstring>& seen)
{
    if (!DirectoryExists(dir)) return;
    std::wstring full = FullPath(dir);
    std::wstring firDir = ParentDir(ParentDir(full));
    std::wstring fir = UpperW(firDir.substr(firDir.find_last_of(L"\\/") + 1));
    if (!seen.insert(L"FIR:" + fir).second) return;
    seen.insert(UpperW(full));
    out.push_back(full);
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
            if (icao.empty()) continue;
            auto& list = index.asrByIcao[icao];
            bool duplicate = std::any_of(list.begin(), list.end(), [&](const std::wstring& existing) {
                return _wcsicmp(existing.substr(existing.find_last_of(L'\\') + 1).c_str(), file.c_str()) == 0;
            });
            if (!duplicate) list.push_back(grp + L"\\" + file);
        }

        // Master ASR of this FIR: <FIR>\ASR\*Master*.asr ("Master.asr", "VECF Master.asr").
        {
            std::vector<std::wstring> masters;
            HANDLE m = FindFirstFileExW((ParentDir(grp) + L"\\*master*.asr").c_str(), FindExInfoBasic, &fd,
                                        FindExSearchNameMatch, nullptr, 0);
            if (m != INVALID_HANDLE_VALUE) {
                do {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) masters.push_back(fd.cFileName);
                } while (FindNextFileW(m, &fd));
                FindClose(m);
            }
            if (!masters.empty()) {
                std::sort(masters.begin(), masters.end(), [](const std::wstring& a, const std::wstring& b) {
                    // prefer the shortest name ("Master.asr" over "Master old.asr")
                    return a.size() != b.size() ? a.size() < b.size() : _wcsicmp(a.c_str(), b.c_str()) < 0;
                });
                index.masterByGrp[grp] = ParentDir(grp) + L"\\" + masters.front();
            }
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

} // namespace adl
