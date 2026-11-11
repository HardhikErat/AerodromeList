#pragma once
#include <windows.h>
#include <string>

// Where EuroScope's own "Metars" list drew each station's row.
//
// The plug-in SDK cannot see EuroScope's lists. For every row of the Metars
// list EuroScope creates a clickable screen element from the station name and
// the row rectangle (the same rectangle it uses for the mouse-over box). That
// small factory function is located by signature (through the RTTI of
// CMetarScreenElement) and detoured, so each refresh records station -> row.
// If anything does not match, Available() is false and nothing is patched.
namespace MetarHighlight {

bool Initialise(std::string& diagnostic);
bool Available();
void Shutdown(); // restores EuroScope's code; call before the DLL unloads

// Start of a radar refresh: rows recorded before this are stale.
void BeginFrame();

// Row of `icao` in the Metars list drawn during the current refresh.
bool RowRect(const std::string& icao, RECT& rc);

} // namespace MetarHighlight
