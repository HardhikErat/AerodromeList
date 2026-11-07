#pragma once
#include <windows.h>

#include <map>
#include <string>
#include <vector>

namespace adl {

struct AsrIndex {
    std::map<std::string, std::vector<std::wstring>> asrByIcao; // full paths, preferred first
    std::map<std::string, std::string> airportNames;
    std::vector<std::wstring> grpFolders;                      // closest to the plug-in first
    std::map<std::wstring, std::wstring> masterByGrp;          // GRP folder -> <FIR>\ASR\*Master*.asr
};

// Scans for <FIR>\ASR\GRP folders near the plug-in and indexes their ASR files.
AsrIndex BuildIndex(const std::wstring& pluginDir, const std::vector<std::wstring>& extraFolders);

// Reads a file once so that it sits in the OS file cache.
void PrewarmFile(const std::wstring& path);

std::string Narrow(const std::wstring& w);
std::wstring Widen(const std::string& s);
std::string Upper(std::string s);
std::wstring UpperW(std::wstring s);
std::wstring ParentDir(const std::wstring& path);
std::wstring ModulePath();

} // namespace adl
