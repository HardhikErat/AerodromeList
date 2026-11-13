# Aerodrome List – EuroScope plug-in

Author: Hardhik Erat

A small window that lists every aerodrome in the loaded sector file. Click an
aerodrome to open its ASR from `<FIR>\ASR\GRP` (VABF, VOMF, VIDF, VECF). If that
ASR is already open, EuroScope switches to it instead of opening a second copy.

## Install

1. Copy `build\AerodromeList.dll` into the FIR's plug-in folder, for example
   `VABF\Plugins\AerodromeList\AerodromeList.dll`.
2. In EuroScope, go to **Other Set → Plug-ins… → Load** and pick the DLL.
3. With Aerodrome List selected in that dialog, move **Standard ES radar
   screen** into "Allowed to draw on types". This is only needed for the METAR
   highlight (below). Then save your profile so it loads every time, and
   restart EuroScope.

The plug-in looks for `<FIR>\ASR\GRP` folders by walking up from its own
folder, from the folders of the other loaded plug-ins (TopSky, GRplugin and so
on) and from EuroScope's working folder. Near each of those it also looks one or
two levels into neighbouring folders, so the four `*-Install_*` folders side by
side are all found, wherever the DLL itself lives. Only the closest copy of
each FIR is used, so older copies of the sector files elsewhere are ignored. If
the window title says "no ASR folder found", add the folder in
`AerodromeList.ini`:

```ini
[Folders]
AsrFolders=D:\Some\Path\VABF\ASR\GRP;D:\Another\SectorRoot
```

## Use

The window is styled like EuroScope's own lists. It lists the aerodromes of the
loaded sector file that have an ASR in the GRP folders. Each row shows just the
pin, the ICAO code and the active runways; right-click a row to see the
aerodrome's full name.

| Action | Result |
|---|---|
| Click an aerodrome | Opens its ASR, or switches to it if it's already open |
| Click the pin in front of an aerodrome | Pins it to the top of the list (click again to unpin) |
| Drag a pinned aerodrome up or down | Changes the order of the pinned aerodromes |
| Type in the search box | Filters by ICAO code or name; **↓** moves into the list |
| Type a full ICAO and press **Enter** | Opens that aerodrome's ASR straight away |
| **Master** button | Goes back to the FIR's Master ASR (`<FIR>\ASR\*Master*.asr`) |
| ▲ on the title strip (or double-click the title) | Collapses the window to its title strip |
| Right-click an aerodrome | Shows its full name in a small dark box (and lets you pick an ASR if it has more than one) |
| Green ICAO | The ASR you last opened from the list |
| Blue text at the right of a row | That aerodrome's active runways: `27` (both ways) or `27/14` (departure/arrival) |

Drag the title strip to move the window, and drag its edges to resize it.

### Active runways

Useful for enroute and approach controllers who need to set runways at many
aerodromes quickly. It works for every aerodrome of the loaded sector file,
including ones without an ASR.

| Action | Result |
|---|---|
| Type a full ICAO in the search box, or click the runway cell at the right of a row | Opens that aerodrome's runway panel at the bottom of the window |
| Click **DEP** / **ARR** next to a runway | Makes that runway active (or inactive) for departures / arrivals |
| Click **DEP** / **ARR** next to **APT** | Makes the aerodrome itself active (or inactive) for departures / arrivals |
| Type `VABB 27` in the search box and press **Enter** | Runway 27 becomes the only active runway, both ways |
| Type `VABB 27/14` and press **Enter** | Departures on 27, arrivals on 14 (lists like `09,14/27` and `27/-` work too) |
| Type `VABB off` and press **Enter** | VABB and all its runways become inactive |
| **...** in the panel | Opens EuroScope's own active airport/runway dialog at that aerodrome |
| **x** in the panel | Closes the panel |

Turning a runway on also turns the aerodrome on for that direction.

Changes go through EuroScope's own "Active airport/runway selector dialog". The
plug-in opens it hidden, ticks the same boxes you would, and presses OK. So
EuroScope applies the change exactly as if you had used the dialog: it shows up
in that dialog and in the ES runway settings, SIDs/STARs are updated, other
plug-ins (TopSky, Ground Radar) are told, and EuroScope saves the `.rwy` file
the same way it always does.

### Automatic aerodromes and runways when you open a position

When you connect as a controller (for example `VABF_CTR` or `VABB_APP`), the
plug-in selects the aerodromes of your sector and sets their runways from the
real-world wind:

1. Your callsign is looked up in the sector's `.ese` file (`[POSITIONS]`).
2. Every aerodrome of the sector file that lies inside a sector your position
   owns or covers (its ID in the sector's `OWNER:` list) is selected, plus the
   aerodrome in the callsign itself (VABB for `VABB_TWR`). `VABF_CTR` gets the
   whole FIR; `VABB_APP` gets VABB, Juhu and Navi Mumbai.
3. Their real METARs are fetched in one request from VATSIM's METAR service
   (the one EuroScope uses), in the background.
4. For each aerodrome the main (longest) runway's end with the most headwind is
   made active for departures and arrivals; parallel runways in the same
   direction come along, and a better-aligned runway is used if the main one has
   more than 20 kt crosswind. With calm, variable or very light wind the runway
   that is already active is kept; without a METAR (or with nothing active in
   calm wind) the main runway is used.
5. Everything is applied in one go through EuroScope's own runway dialog, and a
   summary appears in the *Aerodrome List* message tab. The reason for each
   choice, with its METAR, is written to `AerodromeList-auto.txt` next to the DLL.

This happens once per position per EuroScope session, so your own changes
afterwards are left alone. Aerodromes outside your sector are not changed.

If the sector file draws a runway with the wrong length, tell the plug-in which
runway is the main one in `AerodromeList.ini` (the end you name is also used in
calm wind):

```ini
[MainRunway]
VAJJ=08
```

Parallel runways in the same direction are turned on together (VOBL 09L+09R,
VIDP 28/29L/29R/27), also when light wind keeps the current direction.

Some aerodromes use only one of their runways. These limits from the India
vACC briefing are built in:

* VOHS: only 09R/27L (09L/27R is used as a taxiway)
* VECC: only 01R/19L (01L/19R only during maintenance)
* VOMM: only 07/25 (12/30 only during maintenance)

The wind still picks the direction, but only these runways are used. To change
one, for example while a runway is closed, or to add another aerodrome, use
`[UseRunways]` in `AerodromeList.ini` (`ALL` removes a limit). The plugin reads
this when EuroScope starts:

```ini
[UseRunways]
VECC=01L/19R
VOMM=ALL
VABB=09/27,14/32
```

### METARs of the selected aircraft

Click an aircraft (its symbol or tag) and its departure and destination
METARs get a yellow box in EuroScope's own **Metars** list, so you don't have to
search for them. Only aerodromes in the sector of the position you have open
are boxed (the same aerodromes as above):

* both in your sector: both METARs are boxed
* one in your sector: only that one
* neither: nothing is boxed

The box follows the list when you move it, and goes away when you deselect the
aircraft. A METAR only shows in the Metars list once EuroScope shows that
station there (for example an active aerodrome). If you see a message asking
for it, allow the plug-in to draw on "Standard ES radar screen" (Install, step
3). Like the ASR loader, the Metars list is found in `EuroScope.exe` by
signature; if a future version doesn't match, there's simply no box.

Commands for the EuroScope command line:

* `.adl`: hide or show the window for this session (it always reappears when EuroScope starts)
* `.adl VABB`: open that aerodrome's ASR
* `.adl rwy VABB 27`, `.adl rwy VABB 27/14`, `.adl rwy VABB off`: set active runways
* `.adl rwy VABB`: show VABB in the runway panel
* `.adl auto`: select your position's aerodromes and runways again now
* `.adl auto VABB_APP`: do it as if you had opened that position (also works offline)
* `.adl auto off` / `.adl auto on`: switch the automatic selection on position open off or on
* `.adl hl VABB VOMM`: box METARs as if an aircraft from VABB to VOMM were selected (a test); `.adl hl` stops
* `.adl master`: go back to the Master ASR
* `.adl top`: make the window stay on top of all apps (by default it stays on top of EuroScope only)
* `.adl reload`: scan the ASR folders again (after you add or rename ASR files)
* `.adl info`: write `AerodromeList-info.txt` next to the DLL, listing the ASR folders found, the Master ASR and the open ASRs

Everything you change in the list is saved as you go, in `AerodromeList.ini`
next to the DLL. That covers the pinned aerodromes and their order, the window's
position and size, whether it is collapsed, how far the list is scrolled, and
which aerodrome is in the runway panel.
Changes are written a fraction of a second after you make them, not only when
EuroScope closes. So when you close EuroScope (or it crashes) and open the
sector files again, the list comes back exactly as you left it. Each FIR's copy
of the DLL keeps its own settings. The window has no close button, so it can't
be closed by accident.

## How it stays fast

* ASR folders are scanned once on a background thread at start-up. Every ASR
  is then pre-read into the Windows file cache, so a click loads from memory.
* Clicking calls EuroScope's own "open or switch to ASR" routine directly
  (the one behind the F1+digit ASR fast keys). There is no file dialog and
  no simulated keystrokes.

The EuroScope SDK has no "open ASR" function. At load time the plug-in finds
that routine by matching a byte pattern in `EuroScope.exe` and checks it
against several markers. If a future EuroScope version doesn't match, the
plug-in never calls it. Instead, a click copies the ASR path to the clipboard
and shows a message. The runway dialog is found and checked the same way; if
it doesn't match, the runway controls tell you to use OTHER SET > Active
airports/runways instead. Tested on EuroScope 3.2.12.3 (file version 3.2.13.0).

## Build

Run `build.bat`. It needs Visual Studio 2019 or 2022 with the C++ tools. The
output is a 32-bit DLL with the static runtime, so no redistributables are
needed.
