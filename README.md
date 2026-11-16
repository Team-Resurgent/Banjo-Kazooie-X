# Banjo-Kazooie for the original Xbox

A native original-Xbox port of *Banjo-Kazooie*, built from
[Lighthouse](https://github.com/HarbourMasters/Lighthouse) and its engine,
[libultraship](https://github.com/Kenix3/libultraship). The game is Lighthouse's, compiled for the Xbox's
Pentium III; what it runs on is in `xbox/` and libultraship's Xbox backends.

**No game data is included.** You need your own Banjo-Kazooie ROM. Everything taken from it is made on your own
machine.

| ROM | SHA-1 (`.z64`) | |
| --- | --- | --- |
| USA (Rev0) | `1fe1632098865f639e22c11b9a81ee8f29c75d7a` | not yet tested |
| USA (Rev1), PAL, Japan | as Lighthouse's `config.yml` lists them | not yet tested |

**State:** the port builds; it has not run on an Xbox yet.

## Features

* The full game, with Lighthouse's fixes and enhancements
* An in-game menu on **Back**: fully remappable controls, options and cheats
* Options for cutscene skips, difficulty, keeping notes, Jinjos and extra lives, the follow camera and free look,
  Lighthouse's bug fixes, draw distance and sound volumes
* Rumble, with full, medium and low strengths

Not yet: the frames between the game's own 30 a second (60 fps), each save file as its own Xbox save game (saves are
kept in the game's folder, `E:\TDATA\54520022\saves`), and the dashboard pictures.

## Requirements

* Windows 10 or 11
* the RXDK toolchain: in Visual Studio, **RXDK > RXDK Window > Install Prerequisites**
  (or RXDK's own installer), in `C:\ProgramData\RXDK`
* [Python](https://www.python.org/downloads/) 3.8 or later
* [Git](https://git-scm.com/download/win)
* [Visual Studio](https://visualstudio.microsoft.com/) 2022 or later with **Desktop
  development with C++** (it includes **C++ CMake tools for Windows**, which build
  Torch, the asset extractor; the free Build Tools edition is enough)
* an internet connection the first time you build: [Torch](https://github.com/HarbourMasters/Torch) and the
  libraries the game is built with are downloaded, each at one exact commit, so nothing that changes upstream changes
  this build

`python setup.py --check` lists what is there and what is missing.

## Building

1. Download this repository (**Code > Download ZIP**, or `git clone`).
2. Put your ROM in its folder, named `baserom.z64` (a byte-swapped `.v64` or `.n64` is put
   in order for you).
3. Double-click **`build.bat`**, or in a console:

```bash
python setup.py
```

The result is **`out\Release\XISO\Banjo-Kazooie-X.iso`**. The first build takes a while
(Torch, the extraction and the game are all built); later ones only redo what changed.

The build runs, in order:

1. checks the ROM (SHA-1) against the versions Lighthouse extracts and puts it in `.z64` order (`build/rom/`);
2. fetches Torch at the commit Lighthouse pins (`build/upstream/torch`) and the libraries (`deps/`);
3. builds Torch with Visual Studio (`build/extract/`);
4. extracts the game from your ROM into `data/bk.o2r` with Lighthouse's asset definitions, and packs Lighthouse's own
   files into `data/lighthouse.o2r`;
5. builds the game with the RXDK (`tools/build.py`) and packs the disc.

```bash
python setup.py --check         # what is present and what is missing
python setup.py --skip-build    # everything but the game's compile
```

The build also has what a test run needs on a retail console with nothing listening, off
until it's turned on from the port menu's Debug page:

* **Logging**: a log file and a black box on the hard disk that survive a crash or a
  power-off, and a report of the last run's end at the next start. Off, nothing is written.
  `tools\pull_logs.ps1` copies the files off the console over its FTP server.
* **Test overlay**: the frame rate, memory and timings on screen.

## Running

* **Disc image:** boot `out\Release\XISO\Banjo-Kazooie-X.iso` on a modded console or in
  [xemu](https://xemu.app/).
* **Hard disk:** copy the folder `out\Release\Build\Banjo-Kazooie-X\` (`default.xbe`, the
  two `.o2r` archives and the menu font) to your Xbox and launch `default.xbe`.

## What is in this repository

* `Lighthouse/` -- Lighthouse's game code (`src`, `include`, `lib/ultralib`), its asset definitions
  (`assets/yaml`, `config.yml`) and its own files (`port/`), with the Xbox port's changes
* `libultraship/` -- libultraship at the commit Lighthouse uses (its `bk-lighthouse` branch), with its Xbox port:
  the Direct3D 8 renderer, DirectSound, the Xbox controllers and file system
* `xbox/` -- the Xbox side: `main`, the port's menu, the test tools, and what the game asks of the parts left out
  (the PC menus, online play, mods)
* `tools/` -- `build.py` (the game's build with the RXDK's clang), `fetch_deps.py`, `make_icons.py`,
  `pull_logs.ps1`
* `setup.py`, `build.bat` -- from the ROM to the disc

## Credits

* [Lighthouse](https://github.com/HarbourMasters/Lighthouse) by HarbourMasters, from the
  [Banjo-Kazooie decompilation](https://gitlab.com/banjo.decomp/banjo-kazooie)
* [libultraship](https://github.com/Kenix3/libultraship) and [Torch](https://github.com/HarbourMasters/Torch)
* the RXDK, by Team Resurgent
