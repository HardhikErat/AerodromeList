#pragma once
#include <map>
#include <string>
#include <vector>

// Sets active airports/runways through EuroScope's own
// "Active airport/runway selector dialog".
//
// The EuroScope plug-in SDK can only read runway activity, not set it. This
// opens EuroScope's real dialog (hidden), sets the same cells a controller
// would click, and presses OK, so EuroScope applies the change with its own
// code: the .rwy file is saved, SIDs/STARs are updated and every plug-in gets
// OnAirportRunwayActivityChanged. The dialog code is located by signature
// scanning and validated; if anything does not match, Available() is false.
namespace RunwayDialog {

struct Flags {
    bool dep = false;
    bool arr = false;
};

struct AirportState {
    Flags airport;                        // "active airport" DEP/ARR
    std::map<std::string, Flags> runways; // runway end name ("09", "27L") -> DEP/ARR
};

enum class Result { Applied, NotAvailable, AirportNotListed, Failed };

bool Initialise(std::string& diagnostic);
bool Available();

// Sets the airport's and the listed runways' flags, then confirms with OK.
// Runways of the airport not in the map are left as they are.
// Must be called on EuroScope's UI thread.
Result Apply(const std::string& icao, const AirportState& wanted);

// The same for several aerodromes in one pass of the dialog (keys: ICAO).
// `notListed` receives the aerodromes that were not in EuroScope's list.
Result ApplyMany(const std::map<std::string, AirportState>& wanted, std::vector<std::string>* notListed = nullptr);

// Opens the dialog normally for the controller, scrolled to the airport (optional).
bool Show(const std::string& icao);

// Writes the dialog's rows to a text file (diagnostics).
bool Dump(const std::wstring& file);

} // namespace RunwayDialog
