#include "AutoRunways.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace AutoRunways {

namespace {

const double kPi = 3.14159265358979323846;

std::string Upper(std::string s)
{
    for (char& c : s) c = (char)toupper((unsigned char)c);
    return s;
}

std::vector<std::string> Split(const std::string& s, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

std::string Trim(std::string s)
{
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && isspace((unsigned char)s[i])) ++i;
    return s.substr(i);
}

// "N019.05.30.001" / "E072.51.57.999" -> degrees
bool ParseDms(const std::string& in, double& out)
{
    std::string s = Trim(in);
    if (s.size() < 4) return false;
    char h = (char)toupper((unsigned char)s[0]);
    if (h != 'N' && h != 'S' && h != 'E' && h != 'W') return false;
    std::vector<std::string> p = Split(s.substr(1), '.');
    if (p.size() < 3) return false;
    double deg = atof(p[0].c_str()), min = atof(p[1].c_str());
    double sec = atof((p[2] + "." + (p.size() > 3 ? p[3] : "0")).c_str());
    out = deg + min / 60.0 + sec / 3600.0;
    if (h == 'S' || h == 'W') out = -out;
    return true;
}

struct Edge {
    LatLon a, b;
};

// Even-odd ray cast over a set of edges (the sector's border lines need not be ordered).
bool Inside(const LatLon& p, const std::vector<Edge>& edges)
{
    bool in = false;
    for (const Edge& e : edges) {
        if ((e.a.lat > p.lat) != (e.b.lat > p.lat)) {
            double x = e.a.lon + (p.lat - e.a.lat) * (e.b.lon - e.a.lon) / (e.b.lat - e.a.lat);
            if (p.lon < x) in = !in;
        }
    }
    return in;
}

// Circle sector line approximated by a polygon.
std::vector<LatLon> CirclePoints(const LatLon& c, double radiusNm)
{
    std::vector<LatLon> pts;
    double dLat = radiusNm / 60.0;
    double dLon = radiusNm / (60.0 * std::max(0.01, cos(c.lat * kPi / 180.0)));
    for (int i = 0; i <= 72; ++i) {
        double a = i * 5.0 * kPi / 180.0;
        pts.push_back(LatLon{c.lat + dLat * cos(a), c.lon + dLon * sin(a)});
    }
    return pts;
}

int RunwayNumberHeading(const std::string& end)
{
    int n = atoi(end.c_str());
    return n > 0 && n <= 36 ? n * 10 : 0;
}

double AngleDiff(double a, double b)
{
    double d = fmod(fabs(a - b), 360.0);
    return d > 180 ? 360 - d : d;
}

} // namespace

// --- .ese ----------------------------------------------------------------------------

bool LoadEse(const std::wstring& file, EseData& out)
{
    out = EseData();
    out.file = file;
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    std::string buf((size_t)size.QuadPart, '\0');
    DWORD read = 0;
    ReadFile(h, &buf[0], (DWORD)buf.size(), &read, nullptr);
    CloseHandle(h);
    buf.resize(read);

    // circles are resolved later (their centre may be an airport)
    struct PendingCircle {
        std::string id, a, b;
        double radius;
    };
    std::vector<PendingCircle> circles;

    std::istringstream in(buf);
    std::string line, section, curLine;
    EseData::Sector* curSector = nullptr;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == ';') continue;
        if (line[0] == '[') {
            section = Upper(Trim(line));
            curLine.clear();
            curSector = nullptr;
            continue;
        }
        std::vector<std::string> f = Split(line, ':');
        if (section == "[POSITIONS]") {
            if (f.size() > 6) {
                EseData::Position p;
                p.id = Upper(Trim(f[3]));
                p.prefix = Upper(Trim(f[5]));
                p.type = Upper(Trim(f[6]));
                out.positions[Upper(Trim(f[0]))] = p;
            }
        } else if (section == "[AIRSPACE]") {
            const std::string key = Upper(f[0]);
            if (key == "SECTORLINE" && f.size() > 1) {
                curLine = Trim(f[1]);
                out.lines[curLine];
                curSector = nullptr;
            } else if (key == "CIRCLE_SECTORLINE" && f.size() > 3) {
                // CIRCLE_SECTORLINE:id:ICAO:radius  or  CIRCLE_SECTORLINE:id:lat:lon:radius
                PendingCircle c;
                c.id = Trim(f[1]);
                c.a = Trim(f[2]);
                c.b = f.size() > 4 ? Trim(f[3]) : "";
                c.radius = atof((f.size() > 4 ? f[4] : f[3]).c_str());
                circles.push_back(c);
                curLine.clear();
            } else if (key == "COORD" && f.size() > 2 && !curLine.empty()) {
                LatLon p;
                if (ParseDms(f[1], p.lat) && ParseDms(f[2], p.lon)) out.lines[curLine].push_back(p);
            } else if (key == "SECTOR") {
                out.sectors.emplace_back();
                curSector = &out.sectors.back();
                curLine.clear();
            } else if (key == "OWNER" && curSector) {
                for (size_t i = 1; i < f.size(); ++i)
                    if (!Trim(f[i]).empty()) curSector->owners.push_back(Upper(Trim(f[i])));
            } else if (key == "BORDER" && curSector) {
                for (size_t i = 1; i < f.size(); ++i)
                    if (!Trim(f[i]).empty()) curSector->borders.push_back(Trim(f[i]));
            }
        }
    }
    // Circles with explicit coordinates; ones centred on an airport are kept by name.
    for (const PendingCircle& c : circles) {
        LatLon centre;
        if (!c.b.empty() && ParseDms(c.a, centre.lat) && ParseDms(c.b, centre.lon)) {
            out.lines[c.id] = CirclePoints(centre, c.radius);
        } else {
            out.airportCircles[c.id] = {Upper(c.a), c.radius}; // resolved in SectorAerodromes
        }
    }
    out.loaded = !out.positions.empty() && !out.sectors.empty();
    return out.loaded;
}

std::set<std::string> SectorAerodromes(const EseData& ese, const std::string& callsignIn,
                                       const std::map<std::string, LatLon>& airports, std::string& positionId,
                                       int& sectorCount)
{
    std::set<std::string> result;
    positionId.clear();
    sectorCount = 0;
    std::string callsign = Upper(callsignIn);
    auto pos = ese.positions.find(callsign);
    if (pos == ese.positions.end() || pos->second.id.empty()) return result;
    positionId = pos->second.id;

    for (const EseData::Sector& s : ese.sectors) {
        if (std::find(s.owners.begin(), s.owners.end(), positionId) == s.owners.end()) continue;
        ++sectorCount;
        std::vector<Edge> edges;
        for (const std::string& b : s.borders) {
            std::vector<LatLon> pts;
            auto l = ese.lines.find(b);
            auto c = ese.airportCircles.find(b);
            if (l != ese.lines.end()) {
                pts = l->second;
            } else if (c != ese.airportCircles.end()) {
                auto apt = airports.find(c->second.first);
                if (apt == airports.end()) continue;
                pts = CirclePoints(apt->second, c->second.second);
            }
            for (size_t i = 1; i < pts.size(); ++i) edges.push_back(Edge{pts[i - 1], pts[i]});
        }
        if (edges.empty()) continue;
        for (const auto& a : airports)
            if (Inside(a.second, edges)) result.insert(a.first);
    }
    // VABB_TWR / VABB_APP always include VABB itself
    if (airports.count(pos->second.prefix)) result.insert(pos->second.prefix);
    return result;
}

// --- METAR ---------------------------------------------------------------------------

std::map<std::string, std::string> FetchMetars(const std::vector<std::string>& icaos)
{
    std::map<std::string, std::string> result;
    if (icaos.empty()) return result;

    HINTERNET session = WinHttpOpen(L"AerodromeList EuroScope plug-in", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        session = WinHttpOpen(L"AerodromeList EuroScope plug-in", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return result;
    WinHttpSetTimeouts(session, 5000, 5000, 8000, 8000);
    HINTERNET conn = WinHttpConnect(session, L"metar.vatsim.net", INTERNET_DEFAULT_HTTPS_PORT, 0);

    const size_t kChunk = 25;
    for (size_t start = 0; conn && start < icaos.size(); start += kChunk) {
        std::wstring path = L"/metar.php?id=";
        for (size_t i = start; i < std::min(icaos.size(), start + kChunk); ++i) {
            if (i > start) path += L",";
            for (char c : icaos[i]) path.push_back((wchar_t)(unsigned char)c);
        }
        HINTERNET req = WinHttpOpenRequest(conn, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!req) continue;
        std::string body;
        if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
                std::string chunk(avail, '\0');
                DWORD got = 0;
                if (!WinHttpReadData(req, &chunk[0], avail, &got) || got == 0) break;
                body.append(chunk, 0, got);
            }
        }
        WinHttpCloseHandle(req);

        std::istringstream lines(body);
        std::string l;
        while (std::getline(lines, l)) {
            l = Trim(l);
            if (l.size() < 8) continue;
            std::string icao = Upper(l.substr(0, l.find(' ')));
            if (icao.size() == 4) result[icao] = l;
        }
    }
    if (conn) WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return result;
}

Wind ParseWind(const std::string& metar)
{
    Wind w;
    std::istringstream in(metar);
    std::string t;
    while (in >> t) {
        std::string u = Upper(t);
        double factor = 0;
        size_t unit = 0;
        if (u.size() > 2 && u.compare(u.size() - 2, 2, "KT") == 0) factor = 1.0, unit = 2;
        else if (u.size() > 3 && u.compare(u.size() - 3, 3, "MPS") == 0) factor = 1.944, unit = 3;
        else if (u.size() > 3 && u.compare(u.size() - 3, 3, "KMH") == 0) factor = 0.54, unit = 3;
        if (!factor) continue;
        std::string body = u.substr(0, u.size() - unit);
        if (body.size() < 5) continue;
        std::string dir = body.substr(0, 3);
        if (dir != "VRB" && !(isdigit((unsigned char)dir[0]) && isdigit((unsigned char)dir[1]) && isdigit((unsigned char)dir[2])))
            continue;
        std::string rest = body.substr(3);
        size_t g = rest.find('G');
        std::string spd = rest.substr(0, g);
        if (spd.empty() || !std::all_of(spd.begin(), spd.end(), [](char c) { return isdigit((unsigned char)c); }))
            continue;
        w.known = true;
        w.variable = dir == "VRB";
        w.direction = w.variable ? 0 : atoi(dir.c_str());
        w.speed = (int)lround(atoi(spd.c_str()) * factor);
        if (g != std::string::npos) w.gust = (int)lround(atoi(rest.substr(g + 1).c_str()) * factor);
        return w;
    }
    return w;
}

// --- runway choice -----------------------------------------------------------------------

std::vector<std::string> Choose(const std::vector<Runway>& runwaysIn, const Wind& wind,
                                const std::vector<std::string>& active, std::string& why,
                                const std::string& preferred)
{
    std::vector<std::string> chosen;
    if (runwaysIn.empty()) {
        why = "no runways in the sector file";
        return chosen;
    }
    std::vector<Runway> runways = runwaysIn;
    for (Runway& r : runways)
        for (int i = 0; i < 2; ++i)
            if (r.heading[i] <= 0 || r.heading[i] > 360) r.heading[i] = RunwayNumberHeading(r.end[i]);

    auto join = [](const std::vector<std::string>& v) {
        std::string s;
        for (const std::string& x : v) s += (s.empty() ? "" : ",") + x;
        return s;
    };
    // Parallel runways in the same direction (09L/09R, 28/29) come along with `chosen`.
    auto addParallels = [&](std::vector<std::string>& chosen) {
        std::vector<std::pair<std::string, int>> base;
        for (const Runway& r : runways)
            for (int i = 0; i < 2; ++i)
                if (std::find(chosen.begin(), chosen.end(), r.end[i]) != chosen.end()) base.push_back({r.end[i], r.heading[i]});
        std::vector<std::string> added;
        for (const Runway& r : runways)
            for (int i = 0; i < 2; ++i) {
                if (std::find(chosen.begin(), chosen.end(), r.end[i]) != chosen.end()) continue;
                for (auto& b : base)
                    if (AngleDiff(r.heading[i], b.second) <= 15.0) {
                        chosen.push_back(r.end[i]);
                        added.push_back(r.end[i]);
                        break;
                    }
            }
        return added;
    };
    auto headwind = [&](int hdg) { return wind.speed * cos((wind.direction - hdg) * kPi / 180.0); };
    auto crosswind = [&](int hdg) { return fabs(wind.speed * sin((wind.direction - hdg) * kPi / 180.0)); };

    // which of the "active" ends exist here
    std::vector<std::pair<std::string, int>> current;
    for (const Runway& r : runways)
        for (int i = 0; i < 2; ++i)
            if (std::find(active.begin(), active.end(), r.end[i]) != active.end())
                current.push_back({r.end[i], r.heading[i]});

    char windText[32];
    if (wind.known) {
        if (wind.variable) sprintf_s(windText, "VRB/%02d", wind.speed);
        else sprintf_s(windText, "%03d/%02d", wind.direction, wind.speed);
    }

    const Runway* main = &runways[0];
    for (const Runway& r : runways)
        if (r.length > main->length) main = &r;
    int preferredEnd = 0;
    for (const Runway& r : runways)
        for (int i = 0; i < 2; ++i)
            if (!preferred.empty() && r.end[i] == preferred) {
                main = &r;
                preferredEnd = i;
            }

    bool calm = !wind.known || wind.speed <= 3 || wind.variable;
    int pickHeading = 0;
    std::string pick;

    if (calm) {
        if (!current.empty()) {
            for (auto& c : current) chosen.push_back(c.first);
            why = std::string(wind.known ? "calm/variable wind " + std::string(windText) : "no METAR") + ", kept " +
                  join(chosen);
            std::vector<std::string> added = addParallels(chosen);
            if (!added.empty()) why += " (+" + join(added) + ")";
            return chosen;
        }
        pick = main->end[preferredEnd];
        pickHeading = main->heading[preferredEnd];
        // A light wind with a direction still says which end has the headwind.
        bool directional = wind.known && !wind.variable && wind.speed > 0;
        if (directional && preferred.empty()) {
            int best = headwind(main->heading[0]) >= headwind(main->heading[1]) ? 0 : 1;
            pick = main->end[best];
            pickHeading = main->heading[best];
            why = std::string("light wind ") + windText + " -> " + pick;
        } else {
            why = std::string(wind.known ? "calm/variable wind " + std::string(windText) : "no METAR") +
                  ", main runway " + pick;
        }
    } else {
        // Light wind: keep what is in use unless it has a real tailwind.
        if (wind.speed <= 6 && !current.empty()) {
            double worst = 1e9;
            for (auto& c : current) worst = std::min(worst, headwind(c.second));
            if (worst >= -2.0) {
                for (auto& c : current) chosen.push_back(c.first);
                why = std::string("light wind ") + windText + ", kept " + join(chosen);
                std::vector<std::string> added = addParallels(chosen);
                if (!added.empty()) why += " (+" + join(added) + ")";
                return chosen;
            }
        }
        int best = headwind(main->heading[0]) >= headwind(main->heading[1]) ? 0 : 1;
        pick = main->end[best];
        pickHeading = main->heading[best];
        // Strong crosswind on the main runway: use a better-aligned one if there is one.
        if (crosswind(pickHeading) > 20.0) {
            for (const Runway& r : runways)
                for (int i = 0; i < 2; ++i)
                    if (headwind(r.heading[i]) >= 0 && crosswind(r.heading[i]) < crosswind(pickHeading) - 5.0) {
                        pick = r.end[i];
                        pickHeading = r.heading[i];
                    }
        }
        why = std::string("wind ") + windText + " -> " + pick;
    }

    chosen.push_back(pick);
    std::vector<std::string> added = addParallels(chosen);
    if (!added.empty()) why += " (+" + join(added) + ")";
    return chosen;
}

} // namespace AutoRunways
