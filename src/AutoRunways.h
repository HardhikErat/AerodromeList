#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

// Picks the aerodromes of a controller position and their runways from the
// real-world wind.
namespace AutoRunways {

// --- sector data from the .ese file -----------------------------------------------

struct LatLon {
    double lat = 0;
    double lon = 0;
};

struct EseData {
    bool loaded = false;
    std::wstring file;
    struct Position {
        std::string id;     // "BFC"
        std::string prefix; // "VABF", "VABB"
        std::string type;   // "CTR", "APP", "TWR"...
    };
    std::map<std::string, Position> positions;              // callsign -> position
    std::map<std::string, std::vector<LatLon>> lines;      // SECTORLINE id -> points
    std::map<std::string, std::pair<std::string, double>> airportCircles; // CIRCLE_SECTORLINE id -> (ICAO, radius nm)
    struct Sector {
        std::vector<std::string> owners; // position ids, primary first
        std::vector<std::string> borders; // SECTORLINE ids
    };
    std::vector<Sector> sectors;
};

bool LoadEse(const std::wstring& file, EseData& out);

// Aerodromes (from `airports`) inside any sector the position owns or covers,
// plus the aerodrome in the callsign's prefix (VABB for VABB_TWR).
std::set<std::string> SectorAerodromes(const EseData& ese, const std::string& callsign,
                                       const std::map<std::string, LatLon>& airports, std::string& positionId,
                                       int& sectorCount);

// --- weather -------------------------------------------------------------------------

// Real-world METARs from VATSIM's METAR service (the one EuroScope uses).
// Blocking: call on a worker thread. Returns ICAO -> METAR text.
std::map<std::string, std::string> FetchMetars(const std::vector<std::string>& icaos);

struct Wind {
    bool known = false;
    bool variable = false; // VRB
    int direction = 0;     // degrees true
    int speed = 0;         // knots
    int gust = 0;          // knots
};
Wind ParseWind(const std::string& metar);

// --- runway choice ---------------------------------------------------------------------

struct Runway {
    std::string end[2];  // "09", "27"
    int heading[2] = {0, 0};
    double length = 0;   // any unit, only compared
};

// Runway ends to make active (for departures and arrivals) given the wind and
// the ends that are active now. `why` explains the choice in a few words.
// `preferred` (optional, e.g. "08" from [MainRunway] in the ini) names the main
// runway when the sector file's lengths are misleading; in calm wind with
// nothing active that end is used.
std::vector<std::string> Choose(const std::vector<Runway>& runways, const Wind& wind,
                                const std::vector<std::string>& active, std::string& why,
                                const std::string& preferred = "");

} // namespace AutoRunways
