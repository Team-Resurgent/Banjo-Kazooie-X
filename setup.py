#!/usr/bin/env python3
"""One command from your Banjo-Kazooie ROM to an Xbox disc image.

    python setup.py                 # everything -> out/Release/XISO/Banjo-Kazooie-X.iso
    python setup.py --check         # say what is there and what is missing
    python setup.py --skip-build    # everything but compiling the game (the archives, the tools)
    python setup.py --hd-packs      # and the HD texture packs, downloaded and put on the disc

Needs: Python 3.8+ (standard library only; the HD packs need Pillow, which
setup.py installs with pip when it's missing), Git, Visual Studio 2022 or later
with "Desktop development with C++" (its CMake builds Torch, the asset
extractor), the RXDK (it builds the game), an internet connection the first
time, and your Banjo-Kazooie ROM in this folder as baserom.z64. A .v64 / .n64
(byte-swapped) dump is accepted and put in order.

Nothing that comes from the ROM is in this repository, and nothing on this
PC but the above is used. In order, into folders .gitignore keeps out:

  1. the ROM, checked (SHA-1) against the versions Lighthouse extracts
     (Lighthouse/config.yml), and put in big-endian order: build/rom/baserom.z64
  2. fetched, each at one exact commit, so a change upstream changes nothing
     here: Torch, the asset extractor Lighthouse uses (build/upstream/torch),
     and the libraries the game is built with (deps/, tools/fetch_deps.py)
  3. Torch, built with Visual Studio: build/extract/Release/torch.exe
  4. data/bk.o2r, extracted from the ROM with Lighthouse's asset definitions
     (Lighthouse/assets/yaml), and data/lighthouse.o2r, Lighthouse's own
     files (Lighthouse/port) packed
  5. the HD texture packs: with --hd-packs, BKHD Loulof and BK Reloaded
     downloaded (build/downloads/, each checked by SHA-256) and their .o2r
     put in packs/; every Lighthouse pack in packs/ (those, or any other
     you put there) made into one the Xbox can hold (tools/hd_pack.py):
     data/mods/<pack>-xbox.o2r
  6. the game, built with the RXDK (tools/build.py): out/<config>/XISO/
     Banjo-Kazooie-X.iso and the same files as a folder, out/<config>/Build/
     Banjo-Kazooie-X/, to copy to an Xbox's hard disk; data/mods/ goes on it
     as mods/
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
NAME = "Banjo-Kazooie-X"
BUILD = os.path.join(ROOT, "build")
DATA = os.path.join(ROOT, "data")
LH = os.path.join(ROOT, "Lighthouse")
ROM_NAMES = ("baserom.z64", "baserom.v64", "baserom.n64", "bk.z64", "bk.v64", "bk.n64")
ROM = os.path.join(BUILD, "rom", "baserom.z64")
# USA (Rev1): the one the port has been played and tested with on the Xbox
TESTED_SHA1 = "ded6ee166e740ad1bc810fd678a84b48e245ab80"

# Torch, at the commit Lighthouse has as its submodule
TORCH_URL = "https://github.com/HarbourMasters/Torch.git"
TORCH_REV = "7f5d55f9de53f96e61c11399cb3066f8dd8e835e"
TORCH_SRC = os.path.join(BUILD, "upstream", "torch")

TOOLS_BUILD = os.path.join(BUILD, "extract")
TORCH_EXE = os.path.join(TOOLS_BUILD, "Release", "torch.exe")
ARCHIVES_STAMP = os.path.join(DATA, "made-from.txt")

# HD texture packs made for Lighthouse on a PC: their sources in packs/ (the ones --hd-packs downloads, and any other
# Lighthouse pack put there), each made into one the Xbox can hold by tools/hd_pack.py, in data/mods/, and from there
# on the disc. Each download is checked against the SHA-256 of the file it was made with.
PACKS_DIR = os.path.join(ROOT, "packs")
DOWNLOADS = os.path.join(BUILD, "downloads")
MODS = os.path.join(DATA, "mods")
PACKS = (
    {
        "name": "BKHD Loulof 2.0.0 by Jeod (the whole game)",
        "page": "https://gamebanana.com/mods/730601",
        "url": "https://gamebanana.com/dl/1858571",
        "file": "bkhd-loulof-200.zip",
        "size": 475517958,
        "sha256": "02c59712d2f30b75f337faf292c32f32af0f91f98bff4311f4c5d07c6657e5a4",
        "o2r": "bkhd-loulof-2.0.0.o2r",
        "notes": "readme.txt",
    },
    {
        "name": "BK Reloaded v0.2.1 by GhostlyDark (the interface)",
        "page": "https://github.com/GhostlyDark/BK-Reloaded",
        "url": "https://github.com/GhostlyDark/BK-Reloaded/releases/download/v0.2.1/bk-reloaded-v0.2.1-lh-o2r-hd.7z",
        "file": "bk-reloaded-v0.2.1-lh-o2r-hd.7z",
        "size": 16755303,
        "sha256": "ec1d533bce08b2b59fe71f60ebe6a6aa0782278ee952077c1a2f6f55a42f40af",
        "o2r": "bk-reloaded-v0.2.1.o2r",
        "notes": "bk-reloaded-v0.2.1.txt",
    },
)


def say(msg):
    print("setup: " + msg, flush=True)


def die(msg):
    print("\nsetup: " + msg, flush=True)
    raise SystemExit(1)


def run(cmd, cwd=None, tries=1, again=None):
    """Run cmd, stopping setup if it fails; with tries > 1 it runs again that many times in all, saying `again`
    first."""
    for attempt in range(1, tries + 1):
        r = subprocess.run(cmd, cwd=cwd)
        if r.returncode == 0:
            return
        if attempt < tries:
            say("%s (try %d of %d)" % (again, attempt + 1, tries))
    die("failed (exit %d): %s" % (r.returncode, " ".join('"%s"' % c if " " in c else c for c in cmd)))


def git_head(path):
    if not os.path.exists(os.path.join(path, ".git")):
        return None
    r = subprocess.run(["git", "-C", path, "rev-parse", "HEAD"], capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else None


def remove_tree(path):
    """shutil.rmtree, through the read-only files git leaves in its object store."""
    def retry(func, p, _exc):
        os.chmod(p, stat.S_IWRITE)
        func(p)
    if os.path.isdir(path):
        shutil.rmtree(path, onerror=retry)


def lh_version():
    with open(os.path.join(LH, "CMakeLists.txt"), encoding="utf-8") as f:
        m = re.search(r"project\(Lighthouse VERSION (\d+\.\d+\.\d+)", f.read())
    return m.group(1)


# ---------------------------------------------------------------- the ROM

def find_rom():
    for n in ROM_NAMES:
        p = os.path.join(ROOT, n)
        if os.path.isfile(p):
            return p
    return None


def rom_in_order(data):
    """The ROM in .z64 (big-endian) order: a .v64 swaps each pair of bytes, a .n64 each four."""
    head = data[:4]
    if head == b"\x80\x37\x12\x40":
        return data
    if head == b"\x37\x80\x40\x12":
        b = bytearray(data)
        b[0::2], b[1::2] = data[1::2], data[0::2]
        return bytes(b)
    if head == b"\x40\x12\x37\x80":
        b = bytearray(data)
        b[0::4], b[1::4], b[2::4], b[3::4] = data[3::4], data[2::4], data[1::4], data[0::4]
        return bytes(b)
    return None


def known_roms():
    """{sha1: name} of the ROMs Lighthouse extracts (its asset definitions' config.yml)."""
    out, sha = {}, None
    with open(os.path.join(LH, "config.yml"), encoding="utf-8") as f:
        for line in f:
            m = re.match(r"^([0-9a-f]{40}):", line)
            if m:
                sha = m.group(1)
            m = re.match(r"^\s+name:\s*(.+)$", line)
            if m and sha:
                out[sha] = m.group(1).strip()
                sha = None
    return out


def rom_ok():
    """(sha1, None) for a ROM in order at build/rom/baserom.z64, or (None, why not)."""
    src = find_rom()
    if src is None:
        return None, "no ROM: put your Banjo-Kazooie ROM in %s as baserom.z64" % ROOT
    with open(src, "rb") as f:
        data = rom_in_order(f.read())
    if data is None:
        return None, "%s is not an N64 ROM (its first bytes are none of the three byte orders)" % os.path.basename(src)
    sha = hashlib.sha1(data).hexdigest()
    if sha not in known_roms():
        return None, ("%s is not a ROM Lighthouse extracts (SHA-1 %s); the USA (Rev1) one, tested here, is %s"
                      % (os.path.basename(src), sha, TESTED_SHA1))
    have = None
    if os.path.isfile(ROM):
        with open(ROM, "rb") as f:
            have = hashlib.sha1(f.read()).hexdigest()
    if have != sha:
        os.makedirs(os.path.dirname(ROM), exist_ok=True)
        with open(ROM, "wb") as f:
            f.write(data)
    return sha, None


# ---------------------------------------------------- Visual Studio, CMake

def find_vs_install():
    """The newest Visual Studio (2022 or later) with the x64 C++ tools, by vswhere, which every Visual Studio installer
    puts in the same place: (its folder, its major version -- 17 for 2022, 18 for 2026), or None."""
    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                           "Microsoft Visual Studio", "Installer", "vswhere.exe")
    if not os.path.isfile(vswhere):
        return None
    r = subprocess.run([vswhere, "-latest", "-products", "*", "-version", "[17.0,",
                        "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                        "-format", "json", "-utf8"],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    try:
        found = json.loads(r.stdout) if (r.returncode == 0 and r.stdout) else []
    except ValueError:
        found = []
    if not found:
        return None
    path = found[0].get("installationPath", "")
    try:
        major = int(found[0].get("installationVersion", "0").split(".")[0])
    except ValueError:
        major = 0
    return (path, major) if os.path.isdir(path) else None


def vs_generator(cmake, major):
    """The Visual Studio generator of `major` this CMake has ("Visual Studio 17 2022"), or None."""
    r = subprocess.run([cmake, "--help"], capture_output=True, text=True, encoding="utf-8", errors="replace")
    for line in r.stdout.splitlines():
        name = line.split("=")[0].strip().lstrip("*").strip()
        if name.endswith("[arch]"):
            name = name[:-len("[arch]")].strip()
        if name.startswith("Visual Studio %d " % major):
            return name
    return None


def find_cmake():
    """(cmake, generator) able to build with the Visual Studio found -- its own CMake first, then one on the PATH -- or
    (a cmake, None) when none knows it, or (None, None) with no CMake at all."""
    vs = find_vs_install()
    cands = []
    if vs:
        p = os.path.join(vs[0], "Common7", "IDE", "CommonExtensions", "Microsoft", "CMake", "CMake", "bin", "cmake.exe")
        if os.path.isfile(p):
            cands.append(p)
    c = shutil.which("cmake")
    if c and all(os.path.normcase(c) != os.path.normcase(o) for o in cands):
        cands.append(c)
    if vs:
        for c in cands:
            gen = vs_generator(c, vs[1])
            if gen:
                return c, gen
    return (cands[0] if cands else None), None


def rxdk_root():
    return os.environ.get("RXDK_ROOT") or os.path.join(os.environ.get("ProgramData") or r"C:\ProgramData", "RXDK")


# ---------------------------------------------------------------- --check

def check():
    ok = True
    py = sys.version_info >= (3, 8)
    say("Python: %d.%d%s" % (sys.version_info[0], sys.version_info[1], "" if py else " (3.8 or later needed)"))
    ok &= py
    git = shutil.which("git")
    say("Git: %s" % (git or "missing (install Git for Windows: https://git-scm.com/download/win)"))
    ok &= git is not None
    rx = rxdk_root()
    clang = os.path.join(rx, "llvm", "xbox-windows-x64", "bin", "clang.exe")
    imagebld = os.path.join(rx, "tools", "imagebld.exe")
    rx_ok = os.path.isfile(clang) and os.path.isfile(imagebld)
    say("RXDK: %s" % (rx if rx_ok else "missing in %s (RXDK > RXDK Window > Install Prerequisites)" % rx))
    ok &= rx_ok
    vs = find_vs_install()
    say("Visual Studio C++ tools: %s" % (vs[0] if vs else
                                          "missing (Visual Studio 2022 or later, Desktop development with C++)"))
    ok &= vs is not None
    cm, gen = find_cmake()
    if cm is None:
        say("CMake: missing (Visual Studio's C++ CMake tools for Windows, or cmake.org)")
    elif gen is None:
        say("CMake: %s does not know this Visual Studio (install a newer CMake)" % cm)
    else:
        say("CMake: %s (%s)" % (cm, gen))
    ok &= gen is not None
    src = find_rom()
    if src is None:
        say("ROM: missing (put your Banjo-Kazooie ROM here as baserom.z64)")
        ok = False
    else:
        sha, why = rom_ok()
        say("ROM: %s, %s" % (os.path.basename(src), known_roms()[sha]) if sha else "ROM: " + why)
        ok &= sha is not None
    say("Torch: %s" % ("fetched" if fetched(TORCH_SRC, TORCH_REV) else "not fetched yet (the build fetches it)"))
    for label, path in (("Torch (built)", TORCH_EXE), ("data/bk.o2r", os.path.join(DATA, "bk.o2r")),
                        ("data/lighthouse.o2r", os.path.join(DATA, "lighthouse.o2r"))):
        say("%s: %s" % (label, "built" if os.path.isfile(path) else "not built yet"))
    packs = sorted(f for f in (os.listdir(PACKS_DIR) if os.path.isdir(PACKS_DIR) else []) if f.lower().endswith(".o2r"))
    say("HD packs (packs/): %s" % (", ".join(packs) if packs else "none (--hd-packs downloads them)"))
    if packs:
        say("Pillow (for the HD packs): %s" % ("there" if have_pillow() else "missing (the build installs it)"))
    say("everything needed is here" if ok else "something is missing (above)")
    return ok


# ------------------------------------------------------------------ steps

def fetched_marker(dst):
    return os.path.join(dst, ".git", "bkx-fetched")


def fetched(dst, rev):
    """dst is a whole checkout of rev: fetch_at finished it (its marker), not just got as far as the commit."""
    try:
        with open(fetched_marker(dst), encoding="utf-8") as f:
            done = f.read().strip()
    except OSError:
        return False
    return done == rev and git_head(dst) == rev


def fetch_once(url, rev, dst):
    """One go at fetch_at's download: False when a step of it failed (a dropped connection)."""
    os.makedirs(dst)
    for cmd in (["git", "init", "-q", dst], ["git", "-C", dst, "remote", "add", "origin", url],
                ["git", "-C", dst, "fetch", "-q", "--depth", "1", "origin", rev],
                ["git", "-C", dst, "checkout", "-q", "FETCH_HEAD"]):
        if subprocess.run(cmd).returncode != 0:
            return False
    return True


def fetch_at(url, rev, dst, tries=3):
    """dst as a shallow checkout of url at exactly rev. A download cut off part way starts again from nothing, up to
    `tries` times in all."""
    if fetched(dst, rev):
        return
    for attempt in range(1, tries + 1):
        remove_tree(dst)
        if fetch_once(url, rev, dst):
            break
        if attempt < tries:
            say("the download failed (the connection dropped?): starting it again (try %d of %d)"
                % (attempt + 1, tries))
    else:
        remove_tree(dst)
        die("could not download %s (the connection?): run setup.py again" % url)
    head = git_head(dst)
    if head != rev:
        die("%s is commit %s, not %s" % (url, head, rev))
    with open(fetched_marker(dst), "w", encoding="utf-8") as f:
        f.write(rev + "\n")


def step_fetch():
    if shutil.which("git") is None:
        die("Git not found (install Git for Windows: https://git-scm.com/download/win)")
    if not fetched(TORCH_SRC, TORCH_REV):
        say("fetching Torch")
        fetch_at(TORCH_URL, TORCH_REV, TORCH_SRC)
    say("fetching the libraries (deps/)")
    # (fetch_deps skips the libraries already there, so a run cut short by the connection can simply run again)
    run([sys.executable, os.path.join(ROOT, "tools", "fetch_deps.py")], tries=3,
        again="a download failed: fetching the libraries again")


def step_tools(jobs):
    vs = find_vs_install()
    if vs is None:
        die("Visual Studio 2022 or later with the C++ tools not found (Desktop development with C++)")
    cmake, gen = find_cmake()
    if cmake is None:
        die("CMake not found (Visual Studio's C++ CMake tools for Windows, or cmake.org)")
    if gen is None:
        die("%s is too old for Visual Studio %d: install a newer CMake, or Visual Studio's C++ CMake tools" %
            (cmake, vs[1]))
    if os.path.isfile(TORCH_EXE):
        say("Torch: built")
        return
    cache = os.path.join(TOOLS_BUILD, "CMakeCache.txt")
    if os.path.isfile(cache):
        with open(cache, encoding="utf-8", errors="replace") as f:
            text = f.read()
        if ("CMAKE_GENERATOR:INTERNAL=%s\n" % gen) not in text:
            say("build/extract was set up for another Visual Studio: starting it again")
            remove_tree(TOOLS_BUILD)
    say("configuring Torch with %s (its libraries are downloaded the first time)" % gen)
    # CMAKE_POLICY_VERSION_MINIMUM: CMake 4 (Visual Studio 2026's) refuses a project that says it works with CMake
    # older than 3.5, as one of Torch's libraries says; this is CMake's own way to build it anyway.
    # tries: CMake git-clones Torch's libraries here, and a connection dropped mid-clone fails the whole step; run again,
    # what was downloaded stays and the rest is fetched.
    run([cmake, "-S", TORCH_SRC, "-B", TOOLS_BUILD, "-G", gen, "-A", "x64",
         "-DCMAKE_GENERATOR_INSTANCE=" + vs[0], "-DCMAKE_POLICY_VERSION_MINIMUM=3.5", "-DCMAKE_BUILD_TYPE=Release"],
        tries=3, again="a download of Torch's libraries failed (the connection dropped?): configuring again")
    say("building it")
    run([cmake, "--build", TOOLS_BUILD, "--config", "Release", "--parallel", str(jobs), "--target", "torch"])
    if not os.path.isfile(TORCH_EXE):
        die("%s was not built" % os.path.relpath(TORCH_EXE, ROOT))


def step_archives(rom_sha):
    """data/bk.o2r and data/lighthouse.o2r, made again when the ROM, the definitions or Torch changed."""
    version = lh_version()
    made_from = "rom %s\ntorch %s %d\nlighthouse %s\n" % (rom_sha, TORCH_REV, int(os.path.getmtime(TORCH_EXE)),
                                                          version)
    have = None
    if os.path.isfile(ARCHIVES_STAMP):
        with open(ARCHIVES_STAMP, encoding="utf-8") as f:
            have = f.read()
    bk, port = os.path.join(DATA, "bk.o2r"), os.path.join(DATA, "lighthouse.o2r")
    if have == made_from and os.path.isfile(bk) and os.path.isfile(port):
        say("data/: the archives are up to date")
        return
    remove_tree(DATA)
    os.makedirs(DATA)
    say("extracting the game from the ROM into data/bk.o2r (a few minutes)")
    # Torch finds config.yml and the definitions it names (assets/yaml/...) in its source folder; -u stamps the
    # archive with Lighthouse's version (portVersion), as Lighthouse's own extractor does
    run([TORCH_EXE, "o2r", ROM, "-s", LH, "-d", DATA, "-u", version], cwd=LH)
    say("packing Lighthouse's own files into data/lighthouse.o2r")
    run([TORCH_EXE, "pack", os.path.join(LH, "port"), port, "o2r", "-u", version], cwd=LH)
    if not (os.path.isfile(bk) and os.path.isfile(port)):
        die("the archives were not made (data/ has %s)" % (", ".join(os.listdir(DATA)) or "nothing"))
    with open(ARCHIVES_STAMP, "w", encoding="utf-8") as f:
        f.write(made_from)


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def download(url, dst, size, sha256, tries=3):
    """url to dst, checked: a download cut off or not the file expected is made again, up to `tries` times in all."""
    import urllib.request

    if os.path.isfile(dst) and os.path.getsize(dst) == size and sha256_of(dst) == sha256:
        return
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    part = dst + ".part"
    for attempt in range(1, tries + 1):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 (Banjo-Kazooie-X setup)"})
            with urllib.request.urlopen(req, timeout=60) as r, open(part, "wb") as f:
                got, shown = 0, -1
                for block in iter(lambda: r.read(1 << 20), b""):
                    f.write(block)
                    got += len(block)
                    tenth = got * 10 // size if size else 0
                    if tenth != shown:
                        shown = tenth
                        print("setup:   %d of %d MB" % (got >> 20, size >> 20), flush=True)
            if os.path.getsize(part) == size and sha256_of(part) == sha256:
                os.replace(part, dst)
                return
            say("%s is not the file expected (changed since this build was made?)" % url)
        except OSError as e:
            say("the download failed (%s)" % e)
        if attempt < tries:
            say("downloading it again (try %d of %d)" % (attempt + 1, tries))
    if os.path.isfile(part):
        os.remove(part)
    die("could not download %s: run setup.py again, or download it yourself and put it in %s" %
        (url, os.path.relpath(os.path.dirname(dst), ROOT)))


def unpack(archive, names, dst):
    """The files `names` out of a .zip or .7z into dst. A .7z with Windows' own tar (libarchive), or 7-Zip."""
    if archive.lower().endswith(".zip"):
        import zipfile
        with zipfile.ZipFile(archive) as z:
            for n in names:
                with z.open(n) as src, open(os.path.join(dst, n), "wb") as out:
                    shutil.copyfileobj(src, out, 1 << 20)
        return
    tar = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "tar.exe")
    seven = os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"), "7-Zip", "7z.exe")
    if os.path.isfile(tar) and subprocess.run([tar, "-xf", archive, "-C", dst] + list(names)).returncode == 0:
        return
    if os.path.isfile(seven) and subprocess.run([seven, "x", "-y", "-o" + dst, archive] + list(names)).returncode == 0:
        return
    die("could not unpack %s: install 7-Zip (https://www.7-zip.org/) and run setup.py again" % archive)


def step_download_packs():
    """--hd-packs: each pack downloaded (build/downloads/) and its .o2r taken out into packs/."""
    os.makedirs(PACKS_DIR, exist_ok=True)
    for p in PACKS:
        o2r = os.path.join(PACKS_DIR, p["o2r"])
        if os.path.isfile(o2r):
            say("HD pack: %s: in packs/" % p["name"])
            continue
        say("HD pack: downloading %s (%d MB) from %s" % (p["name"], p["size"] >> 20, p["page"]))
        archive = os.path.join(DOWNLOADS, p["file"])
        download(p["url"], archive, p["size"], p["sha256"])
        unpack(archive, [p["o2r"], p["notes"]], PACKS_DIR)
        # its notes beside it, named as it is: hd_pack.py puts them in the Xbox pack (the authors' credits)
        notes = os.path.join(PACKS_DIR, p["notes"])
        stem = os.path.splitext(p["o2r"])[0]
        if os.path.isfile(notes) and p["notes"] != stem + ".txt":
            os.replace(notes, os.path.join(PACKS_DIR, stem + ".txt"))
        if not os.path.isfile(o2r):
            die("%s had no %s" % (p["file"], p["o2r"]))


def have_pillow():
    return subprocess.run([sys.executable, "-c", "import PIL"], capture_output=True).returncode == 0


def step_packs():
    """Each pack in packs/ made for the Xbox into data/mods/ (tools/hd_pack.py), again when it, the game's archive or
    the converter changed; an Xbox pack whose source has gone is taken off."""
    sources = sorted(f for f in (os.listdir(PACKS_DIR) if os.path.isdir(PACKS_DIR) else [])
                     if f.lower().endswith(".o2r"))
    made = {os.path.splitext(f)[0] + "-xbox.o2r" for f in sources}
    if os.path.isdir(MODS):
        for f in os.listdir(MODS):
            if f.endswith(("-xbox.o2r", "-xbox.made-from")) and f.replace(".made-from", ".o2r") not in made:
                os.remove(os.path.join(MODS, f))
    if not sources:
        return
    if not have_pillow():
        say("the HD packs need Pillow: installing it (pip install pillow)")
        run([sys.executable, "-m", "pip", "install", "pillow"])
    tool = os.path.join(ROOT, "tools", "hd_pack.py")
    with open(ARCHIVES_STAMP, encoding="utf-8") as f:
        base = f.read()
    for f in sources:
        src = os.path.join(PACKS_DIR, f)
        out = os.path.join(MODS, os.path.splitext(f)[0] + "-xbox.o2r")
        stamp = out[:-len(".o2r")] + ".made-from"
        made_from = "pack %d %d\ntool %s\n%s" % (os.path.getsize(src), int(os.path.getmtime(src)), sha256_of(tool),
                                                 base)
        have = None
        if os.path.isfile(stamp):
            with open(stamp, encoding="utf-8") as s:
                have = s.read()
        if have == made_from and os.path.isfile(out):
            say("HD pack: data/mods/%s is up to date" % os.path.basename(out))
            continue
        say("HD pack: making %s for the Xbox (a few minutes for a big one)" % f)
        run([sys.executable, tool, src, "--out", out])
        with open(stamp, "w", encoding="utf-8") as s:
            s.write(made_from)


def step_build(jobs):
    say("building the game with the RXDK")
    run([sys.executable, os.path.join(ROOT, "tools", "build.py"), "-j", str(jobs)])
    iso = os.path.join(ROOT, "out", "Release", "XISO", NAME + ".iso")
    if not os.path.isfile(iso):
        die("the disc image was not made")
    say("done: %s" % os.path.relpath(iso, ROOT))
    say("      %s (the same files, to copy to a hard disk)" %
        os.path.relpath(os.path.join(ROOT, "out", "Release", "Build", NAME), ROOT))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="say what is there and what is missing, and stop")
    ap.add_argument("--skip-build", action="store_true", help="stop before compiling the game")
    ap.add_argument("--hd-packs", action="store_true",
                    help="download the HD texture packs (about 490 MB) into packs/ and put them on the disc")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()
    if a.check:
        raise SystemExit(0 if check() else 1)
    if sys.version_info < (3, 8):
        die("Python 3.8 or later is needed")
    rom_sha, why = rom_ok()
    if rom_sha is None:
        die(why)
    say("ROM: %s" % known_roms()[rom_sha])
    step_fetch()
    step_tools(a.jobs)
    step_archives(rom_sha)
    if a.hd_packs:
        step_download_packs()
    step_packs()
    if a.skip_build:
        say("stopped before the game's build (--skip-build)")
        return
    step_build(a.jobs)


if __name__ == "__main__":
    main()
