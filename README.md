# Banjo-Kazooie for the original Xbox

A native original-Xbox port of *Banjo-Kazooie* (N64, 1998), built from
[Lighthouse](https://github.com/HarbourMasters/Lighthouse), HarbourMasters' PC port of the
[Banjo-Kazooie decompilation](https://gitlab.com/banjo.decomp/banjo-kazooie), and its engine,
[libultraship](https://github.com/Kenix3/libultraship). The game code is Lighthouse's, compiled for the Xbox's
Pentium III. It runs on libultraship's Xbox backends: NV2A graphics through Direct3D 8, DirectSound audio, Xbox pads
and hard disk saves. It is not emulation, and it runs on stock 64 MB consoles.

**No game data is included.** You need your own Banjo-Kazooie ROM. Everything taken from it is made on your own
machine, and so are the optional HD texture packs, which are downloaded from their authors' pages.

| ROM | SHA-1 (`.z64`) | |
| --- | --- | --- |
| USA (Rev1) | `ded6ee166e740ad1bc810fd678a84b48e245ab80` | tested |
| USA (Rev0) | `1fe1632098865f639e22c11b9a81ee8f29c75d7a` | extracted, not tested |
| PAL, Japan | as Lighthouse's `config.yml` lists them | extracted, not tested |

## Features

* The full game, with Lighthouse's fixes and enhancements
* 60 fps: the frames between the game's own are drawn (Options, Display and sound, **60 fps**)
* 720p, 480p or 480i, following the dashboard's video settings, in 16:9 when the dashboard is set to widescreen
* An in-game menu on **Back**: controls, options, cheats, mods, restart and exit to the dashboard
* Fully remappable controls, rumble (full, medium, low or off)
* Options for cutscene skips, difficulty, keeping notes, Jinjos and extra lives, the follow camera and free look,
  Lighthouse's bug fixes, draw distance and sound volumes
* HD texture packs: [BKHD Loulof](https://gamebanana.com/mods/730601) and
  [BK Reloaded](https://github.com/GhostlyDark/BK-Reloaded) are downloaded and made to fit the Xbox's memory by the
  build when you ask for them (see [HD texture packs](#hd-texture-packs)). The game starts as it was, with every pack
  off, until you turn them on in the menu.
* Saves and settings on the hard disk, in the title's own folder (`E:\TDATA\54520022`)

## Requirements

* Windows 10 or 11
* the RXDK toolchain, in `C:\ProgramData\RXDK`: in Visual Studio, **RXDK > RXDK Window > Install Prerequisites**,
  or RXDK's own installer
* [Python](https://www.python.org/downloads/) 3.8 or later (tick **Add python.exe to PATH**)
* [Git](https://git-scm.com/download/win)
* [Visual Studio](https://visualstudio.microsoft.com/) 2022 or later with **Desktop development with C++**. It
  includes **C++ CMake tools for Windows**, which builds Torch, the asset extractor. The free Build Tools edition is
  enough.
* an internet connection the first time you build. [Torch](https://github.com/HarbourMasters/Torch) and the
  libraries the game is built with are downloaded, each at one exact commit, so nothing that changes upstream changes
  this build.

The HD texture packs also need [Pillow](https://pypi.org/project/Pillow/), which the build installs with `pip` when
it's missing.

`python setup.py --check` lists what is there and what is missing.

## Building

1. Download this repository (**Code > Download ZIP**, or `git clone`).
2. Put your ROM in its folder, named `baserom.z64`. A byte-swapped `.v64` or `.n64` dump is put in order for you.
3. Double-click **`build.bat`**. The first time, it asks whether to download the HD texture packs too (about
   490 MB). Or, in a console:

```bash
python setup.py
```

The result is **`out\Release\XISO\Banjo-Kazooie-X.iso`**. The first build takes a while, since Torch, the extraction
and the game are all built. Later ones only redo what changed.

The build runs, in order:

1. checks the ROM (SHA-1) against the versions Lighthouse extracts and puts it in `.z64` order (`build/rom/`);
2. fetches Torch at the commit Lighthouse pins (`build/upstream/torch`) and the libraries (`deps/`);
3. builds Torch with Visual Studio (`build/extract/`);
4. extracts the game from your ROM into `data/bk.o2r` with Lighthouse's asset definitions, and packs Lighthouse's own
   files into `data/lighthouse.o2r`;
5. with `--hd-packs`, downloads the HD texture packs into `packs/`, then makes every pack in `packs/` into one the
   Xbox can hold (`data/mods/`);
6. builds the game with the RXDK (`tools/build.py`) and packs the disc.

| Command | What it does |
| --- | --- |
| `python setup.py` | everything, from the ROM to the disc |
| `python setup.py --hd-packs` | the same, with the HD texture packs downloaded and put on the disc |
| `python setup.py --check` | what is present and what is missing |
| `python setup.py --skip-build` | everything but the game's compile |
| `python setup.py -j 8` | the compile on 8 cores (all of them by default) |

`build.bat` passes its arguments on to `setup.py` (`build.bat --hd-packs`).

## Running

* **Disc image:** boot `out\Release\XISO\Banjo-Kazooie-X.iso` on a modded console or in [xemu](https://xemu.app/).
* **Hard disk:** copy the folder `out\Release\Build\Banjo-Kazooie-X\` to your Xbox and launch `default.xbe` from
  your dashboard. It holds `default.xbe`, the two `.o2r` archives, the menu font and, with the HD packs, the `mods`
  folder.

## The menu

Press **Back** in the game.

* **Controls**: every N64 button can be put on any Xbox button, and the rumble's strength set.
* **Cheats**: infinite health, air, lives and items; movement (levitate, fly anywhere, Talon Trot cycling); fast and
  D-pad-cycled transformations.
* **Options**: cutscenes, gameplay, saving, camera and controls, Lighthouse's fixes, and display and sound (60 fps,
  draw distance, HD textures, volumes).
* **Mods**: the texture packs on the disc, each off until you turn it on. See below.
* **Debug**: logging and a test overlay, for testing (see [For testers](#for-testers)).
* **Restart game** and **Exit to dashboard**.

## HD texture packs

Two packs made for Lighthouse on a PC can be put on the disc:

| Pack | Author | What it changes | Download |
| --- | --- | --- | --- |
| [BKHD Loulof 2.0.0](https://gamebanana.com/mods/730601) | Jeod | the whole game | 454 MB |
| [BK Reloaded v0.2.1](https://github.com/GhostlyDark/BK-Reloaded) | GhostlyDark | the interface | 16 MB |

`python setup.py --hd-packs` (or **Y** at `build.bat`'s question) downloads them into `build/downloads/`, checks each
against the SHA-256 of the file this build was made with, and puts their `.o2r` files in `packs/`. Once they're there,
every build puts them on the disc. To leave a pack off the disc, delete it from `packs/`.

**Your own packs:** any Lighthouse texture pack (`.o2r`) you put in `packs/` is made for the Xbox and put on the disc
the same way. Its notes, as `<pack>.txt` beside it, go into the Xbox pack with it.

### Why they're converted

A PC pack doesn't fit on a 64 MB Xbox: BKHD is 480 MB of textures at 32 bits a texel. `tools/hd_pack.py` makes each
texture at most twice the game's size on each side, and no larger than 512, the Xbox's largest texture. It keeps 16
bits a texel where the game's own texture had only one bit of alpha, and grey textures as IA16. BKHD comes to 59 MB on
the disc, of which only the textures of the level you're in are loaded.

### In the game: Back, Mods

The game starts stock: every pack is off, and so is **HD textures**. To play with a pack:

1. **Back, Mods**: turn the pack on (**A**, or **left and right**), then **Restart to apply**. Packs are read when
   the game starts, which is why a change needs the restart.
2. **Back, Options, Display and sound, HD textures**: on. This one needs no restart, and turning it off again shows
   the game's own textures at once, to compare.

The Mods list is the packs' order: **where two packs change the same texture, the higher one wins**. **X** moves the
chosen pack up and **Y** down. A pack you add later goes on top the first time. The line under the list says what
the chosen pack is doing (off, loaded, or loaded while HD textures are off).

With both packs on, BKHD (on top until you move it) is used almost everywhere. BK Reloaded adds the 48 textures BKHD doesn't
have. Move BK Reloaded to the top for its interface over BKHD's.

### tools/hd_pack.py

`setup.py` runs it for you. Run it yourself to make a pack with other settings:

```bash
python tools/hd_pack.py packs/bkhd-loulof-2.0.0.o2r
python tools/hd_pack.py packs/bkhd-loulof-2.0.0.o2r --scale 1 --out data/mods/bkhd-loulof-2.0.0-xbox.o2r
```

| Option | |
| --- | --- |
| `PACK` | the Lighthouse pack (`.o2r`) |
| `--out` | where the Xbox pack goes (`data/mods/<pack>-xbox.o2r`) |
| `--scale` | how many times the game's size a texture may be, at most (2). 1 keeps each at the game's size: the pack's art in the least memory |
| `--base` | the game's own archive, which says what each texture is (`data/bk.o2r`) |

It prints how many textures it made, the size they take when all are loaded, and any it left out. Set
`HD_PACK_LIST=1` to list each one left out. Every file in `data/mods/` goes on the disc at the next build.

### Adding a pack by hand, without a new build

A pack you downloaded or made yourself can go straight onto a hard disk install. It must be converted first: a PC
pack copied as it is would not fit in the Xbox's memory.

1. Build the game once (`build.bat`). The converter needs `data/bk.o2r`, made from your ROM, to know what each
   texture is.
2. Convert the pack (Pillow: `pip install pillow`):

   ```bash
   python tools/hd_pack.py C:\path\to\my-pack.o2r --out C:\path\to\my-pack-xbox.o2r
   ```

3. Copy `my-pack-xbox.o2r` into the `mods` folder beside `default.xbe` on the Xbox (over FTP, for example
   `E:\Games\Banjo-Kazooie-X\mods\`). Make the folder if there isn't one.
4. In the game: **Back, Mods**, turn the pack on, **Restart to apply**, then **Back, Options, Display and sound, HD
   textures** on.

A disc image can't be changed afterwards: for the ISO, put the pack in `packs/` and run `build.bat` again.

## For testers

The build has what a test run needs on a retail console with nothing listening. It's off until it's turned on from
the menu's **Debug** page:

* **Logging**: a log file and a black box on the hard disk that survive a crash or a power-off, and a report of the
  last run's end at the next start. Off, nothing is written.
* **Test overlay**: the frame rate, memory and timings on screen.

`tools\pull_logs.ps1` copies the log files off the console over its FTP server:

```bash
powershell -ExecutionPolicy Bypass -File tools\pull_logs.ps1 -Xbox 192.168.1.50
```

## What is in this repository

* `Lighthouse/`: Lighthouse's game code (`src`, `include`, `lib/ultralib`), its asset definitions (`assets/yaml`,
  `config.yml`) and its own files (`port/`), with the Xbox port's changes
* `libultraship/`: libultraship at the commit Lighthouse uses (its `bk-lighthouse` branch), with its Xbox port: the
  Direct3D 8 renderer, DirectSound, the Xbox controllers and file system
* `xbox/`: the Xbox side: `main`, the port's menu, the test tools, and what the game asks of the parts left out (the
  PC menus, online play, the PC's mod manager); `xbox/src/port/lhx_nomods.cpp` reads the Xbox's `mods` folder and
  `lhx_hdprefetch.cpp` reads a pack's textures ahead of the renderer
* `tools/`:
  * `build.py`: the game's build with the RXDK's clang, and the disc (`setup.py` runs it; run it yourself after a
    change to the code: `python tools/build.py`)
  * `fetch_deps.py`: the libraries the game is built with, each at one commit, into `deps/`
  * `hd_pack.py`: a Lighthouse texture pack made into one the Xbox can hold
  * `make_icons.py`: the dashboard pictures, to be made from the game's Jiggy (not finished: the XBE has none yet)
  * `pull_logs.ps1`: a test run's logs, off the console
* `setup.py`, `build.bat`: from the ROM to the disc

`.gitignore` keeps out your ROM and everything made from it (`build/`, `data/`, `out/`, `deps/`) and the texture
packs (`packs/`).

## Credits

* [Lighthouse](https://github.com/HarbourMasters/Lighthouse) by HarbourMasters, from the
  [Banjo-Kazooie decompilation](https://gitlab.com/banjo.decomp/banjo-kazooie)
* [libultraship](https://github.com/Kenix3/libultraship) and [Torch](https://github.com/HarbourMasters/Torch)
* [BKHD Loulof](https://gamebanana.com/mods/730601) by Jeod, and [BK Reloaded](https://github.com/GhostlyDark/BK-Reloaded)
  by GhostlyDark: the HD texture packs, downloaded from their pages at build time, not included here
* the RXDK, by Team Resurgent
