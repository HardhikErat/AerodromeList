# Aerodrome List – EuroScope plug-in

Author: Hardhik Erat

A small window that lists every aerodrome in the loaded sector file. Click an
aerodrome to open its ASR from `<FIR>\ASR\GRP` (VABF, VOMF, VIDF, VECF). If that
ASR is already open, EuroScope switches to it instead of opening a second copy.

## Install

1. Copy `build\AerodromeList.dll` into the FIR's plug-in folder, for example
   `VABF\Plugins\AerodromeList\AerodromeList.dll`.
2. In EuroScope, go to **Other Set → Plug-ins… → Load** and pick the DLL. Then
   save your profile so it loads every time.

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
loaded sector file that have an ASR in the GRP folders.

| Action | Result |
|---|---|
| Click an aerodrome | Opens its ASR, or switches to it if it's already open |
| Click the pin in front of an aerodrome | Pins it to the top of the list (click again to unpin) |
| Drag a pinned aerodrome up or down | Changes the order of the pinned aerodromes |
| Type in the search box | Filters by ICAO code or name; **↓** moves into the list |
| Type a full ICAO and press **Enter** | Opens that aerodrome's ASR straight away |
| **Master** button | Goes back to the FIR's Master ASR (`<FIR>\ASR\*Master*.asr`) |
| ▲ on the title strip (or double-click the title) | Collapses the window to its title strip |
| Right-click an aerodrome | Pin or unpin, or choose between several ASRs (shown as a `+n` badge) |
| Green ICAO | The ASR you last opened from the list |

Drag the title strip to move the window, and drag its edges to resize it.

Commands for the EuroScope command line:

* `.adl`: hide or show the window for this session (it always reappears when EuroScope starts)
* `.adl VABB`: open that aerodrome's ASR
* `.adl master`: go back to the Master ASR
* `.adl top`: make the window stay on top of all apps (by default it stays on top of EuroScope only)
* `.adl reload`: scan the ASR folders again (after you add or rename ASR files)
* `.adl info`: write `AerodromeList-info.txt` next to the DLL, listing the ASR folders found, the Master ASR and the open ASRs

Everything you change in the list is saved as you go, in `AerodromeList.ini`
next to the DLL. That covers the pinned aerodromes and their order, the window's
position and size, whether it is collapsed, and how far the list is scrolled.
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
and shows a message. Tested on EuroScope 3.2.12.3 (file version 3.2.13.0).

## Build

Run `build.bat`. It needs Visual Studio 2019 or 2022 with the C++ tools. The
output is a 32-bit DLL with the static runtime, so no redistributables are
needed.
