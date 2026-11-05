#pragma once
#include <string>

// Opens an ASR file inside the running EuroScope instance.
//
// The EuroScope plug-in SDK has no "open ASR" call, so this locates EuroScope's
// own "open ASR or switch to it if it is already open" routine (the one used by
// the F1+<digit> ASR fast keys) by signature-scanning EuroScope.exe in memory.
// Every step is validated; if anything does not match, Available() returns
// false and nothing is ever called.
namespace AsrOpener {

// Scans EuroScope.exe. Cheap (a few ms); safe to call more than once.
bool Initialise(std::string& diagnostic);

bool Available();

enum class Result { Opened, NotAvailable, Refused, Crashed };

// Must be called on EuroScope's UI thread.
Result Open(const std::string& asrPath);

} // namespace AsrOpener
