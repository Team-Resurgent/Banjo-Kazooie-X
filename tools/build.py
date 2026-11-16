#!/usr/bin/env python3
"""Banjo-Kazooie-X: the original Xbox build of libultraship and Lighthouse.

setup.py runs this once the archives are in data/; run on its own, it rebuilds the game from them.

    python tools/build.py [-k] [-j N] [--only GROUP[,GROUP]] [--file PATTERN]
                          [--no-link] [--clean] [--cmd PATTERN] [--no-lto]

Compiles with the RXDK's clang and the flags its Rxdk.Cli uses (copied from
`Rxdk.Cli build --verbose`), then links, runs imagebld and packs the disc the
way Rxdk.Cli does:

    build/obj/<group>/...      objects, rebuilt when a source, a header it
                               read (clang's depfile) or its command changes
    build/obj-lto/<group>/...  the same for the groups that are ThinLTO bitcode
    build/log/<group>/...      each failed file's compiler output
    build/Banjo-Kazooie-X.exe  the linked PE (with line tables, for symbols)
    build/Banjo-Kazooie-X.xbe  the title
    out/Release/Build/Banjo-Kazooie-X/   the disc's files: default.xbe, the
                               archives (data/*.o2r) and the menu font
    out/Release/XISO/Banjo-Kazooie-X.iso the disc image of them

Groups, in link order: deps (spdlog, tinyxml2, zlib, monocypher, stb), imgui
(Dear ImGui's core, for Lighthouse's calls into it; never drawn), lus
(libultraship minus ImGui, SDL, scripting, MPQ), game (Lighthouse's C: the
decomp in src/ and lib/ultralib), port (Lighthouse's C++), xbox (xbox/src: the
backends and main). Every game and port object is linked (not archived), so
Lighthouse's self-registering hooks (ShipInit) stay in; --gc-sections drops
what nothing reaches.

-k keeps going past failures and lists them all (the porting loop);
--cmd prints the compile command for the files matching PATTERN.
"""
import argparse
import concurrent.futures
import glob
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
LUS = os.path.join(ROOT, "libultraship")
LH = os.path.join(ROOT, "Lighthouse")
DEPS = os.path.join(ROOT, "deps")
XBOX = os.path.join(ROOT, "xbox")
# Torch (setup.py fetches it): its BK64 data structures, for the map reader Lighthouse takes from it
TORCH = os.path.join(BUILD, "upstream", "torch")

RXDK = os.environ.get("RXDK_ROOT") or os.path.join(os.environ.get("ProgramData") or r"C:\ProgramData", "RXDK")
LLVM = os.path.join(RXDK, "llvm", "xbox-windows-x64")
CLANG = os.path.join(LLVM, "bin", "clang.exe")
SDK_INC = os.path.join(RXDK, "sdk", "include")
SDK_LIB = os.path.join(RXDK, "sdk", "lib")
LIBCXX_INC = os.path.join(SDK_INC, "c++", "v1")

TITLE = "Banjo-Kazooie-X"  # the files' names (build/Banjo-Kazooie-X.xbe, out/*/XISO/Banjo-Kazooie-X.iso)
# Lighthouse's commit the Lighthouse/ tree is, for its build stamp
LH_COMMIT = "20e1ca8"
TITLE_NAME = "Banjo-Kazooie-X"  # the name the dashboard shows (the XBE's title name, at most 40 characters)
# Team Resurgent's (TR, 0x5452) number 0x0022 in its list (the TitleID-DB repo). Each title ID has its own T: and U:
# drives, so the configs and saves stay apart from the other ports'.
TITLE_ID = "0x54520022"
STACK = 262144
LIBS = ["libd3d8.lib", "libxgraphics.lib", "libdsound.lib", "libxapi.lib", "libc.lib", "libcpp.lib", "libkernel.lib"]

# ThinLTO, as the other Lus-X ports: across files it finds the C++ that can't throw, which .eh_frame then leaves out.
# deps and xbox stay native: xbox holds what the code generator calls on its own (lusx_string.c), and in bitcode LTO
# would drop it.
LTO_GROUPS = ("lus", "game", "port")
# ...and these as one full-LTO module, not ThinLTO's one a file: the COFF linker keeps every object's .eh_frame whole,
# so the headers' inline functions copied into each ThinLTO module cost unwind records for copies the link dropped
FULL_LTO_GROUPS = ("port",)
LTO = True
# lld imports a function into its callers up to this size; its default of 100 grows .text a lot, which a 64 MB console
# pays for and a 16 KB L1 doesn't repay
LTO_IMPORT_LIMIT = 10


def clang_resource_inc():
    found = sorted(glob.glob(os.path.join(LLVM, "lib", "clang", "*", "include")))
    if not found:
        raise SystemExit("build: no clang resource headers under %s" % LLVM)
    return found[-1]


def builtins_lib():
    found = sorted(glob.glob(os.path.join(LLVM, "lib", "clang", "*", "lib", "windows", "libclang_rt.builtins-i386.a")))
    if not found:
        raise SystemExit("build: no libclang_rt.builtins-i386.a under %s" % LLVM)
    return found[-1]


# ---------------------------------------------------------------- flags

# What Rxdk.Cli passes (RXDK 1.3.9), but -O2 (Rxdk.Cli: -O3; size matters
# here), SSE maths, function/data sections for --gc-sections, and _WIN32
# undefined for C as well as C++: libultraship and Lighthouse then take their
# POSIX paths (picolibc), never <windows.h>.
def rxdk_flags(lang):
    f = []
    if lang == "c++":
        # -fno-assume-unique-vtables: clang 24's exact dynamic_cast to a final class
        # with a virtual base (LUS's controller mappings) asserts in codegen on this target
        f += ["-nostdinc++", "-fexceptions", "-frtti", "-D_LIBCPP_ENABLE_CXX17_REMOVED_AUTO_PTR",
              "-I", LIBCXX_INC, "-fms-compatibility-version=19.20", "-D_GNU_SOURCE", "-fno-assume-unique-vtables"]
    f += ["-U_WIN32", "-U__MINGW32__",
          "-target", "i686-pc-windows-gnu", "-isystem", clang_resource_inc(), "-O2",
          "-fno-sanitize=undefined", "-march=pentium3", "-msse", "-mfpmath=sse", "-mlong-double-64",
          # (not -ffreestanding/-fno-builtin: with them no sqrtf, fabsf, memcpy or memset is ever inlined)
          "-fno-math-errno",
          "-fno-stack-protector", "-fms-extensions", "-fms-compatibility", "-nostdinc",
          "-include", "picolibc.h", "-include", os.path.join(XBOX, "include", "lusx_prelude.h"), "-D_XBOX", "-DXBOX",
          "-U_DEBUG", "-D__ASSERT_VERBOSE",
          "-femulated-tls", "-gline-tables-only", "-ffunction-sections", "-fdata-sections",
          # GCC bitfield layout everywhere: the game's and libultraship's GBI structs must agree
          # (Microsoft's, this target's default, makes Gfx 16 bytes)
          "-mno-ms-bitfields",
          # everything is linked statically: no mingw .refptr indirection for extern data
          "-fno-auto-import",
          "-Wno-c++11-narrowing", "-Wno-address-of-temporary", "-Wno-ignored-pragma-intrinsic", "-Wno-multichar",
          "-Wno-unused-command-line-argument", "-Wno-deprecated-enum-enum-conversion", "-Wno-pragma-pack"]
    f += ["-std=c++20"] if lang == "c++" else ["-std=gnu11"]
    return f


def cmake_sets(path):
    """set(NAME "value") lines of a CMake file, ${VAR}s left for the caller."""
    out = {}
    with open(path, encoding="utf-8") as fh:
        for m in re.finditer(r'^\s*set\(\s*(\w+)\s+"([^"]*)"', fh.read(), re.M):
            out.setdefault(m.group(1), m.group(2))
    return out


def cvar_defines():
    # Lighthouse's names first (its lus-cvars.cmake renames LUS's), then LUS's defaults
    sets = {}
    for p in (os.path.join(LH, "cmake", "lighthouse-cvars.cmake"), os.path.join(LH, "cmake", "lus-cvars.cmake"),
              os.path.join(LUS, "cmake", "cvars.cmake")):
        for k, v in cmake_sets(p).items():
            sets.setdefault(k, v)
    for k, v in sets.items():
        sets[k] = re.sub(r"\$\{(\w+)\}", lambda m: sets.get(m.group(1), ""), v)
    return ['-D%s="%s"' % (k, v) for k, v in sorted(sets.items()) if k.startswith("CVAR_")]


# _LANGUAGE_C: LUS's GBI headers define it for _MSC_VER or __GNUC__, and in
# MS-compatibility mode the RXDK's clang has neither
# (no CONTROLLERBUTTONS_T: Lighthouse has libultraship's, the N64's 16-bit buttons)
COMMON_DEFS = ["-DNDEBUG", "-D_LANGUAGE_C", "-DNOMINMAX", "-DSPDLOG_COMPILED_LIB",
               "-DSPDLOG_ACTIVE_LEVEL=2",  # info and up; trace/debug compiled out
               "-DSPDLOG_NO_THREAD_ID", "-DLUS_XBOX", "-DLUS_NO_IMGUI",
               # Fast3D's texture cache: each entry keeps its GPU texture until the slot is reused
               "-DTEXTURE_CACHE_MAX_SIZE=512",
               "-DLUS_VERSION_EPOCH=2", "-DLUS_VERSION_MAJOR=0", "-DLUS_VERSION_MINOR=0", "-DLUS_VERSION_PATCH=0",
               "-DEXCLUDE_MPQ_SUPPORT"]

# Lighthouse's CMakeLists.txt add_compile_definitions(): set for its directory, so libultraship (its subdirectory)
# is built with them too
LH_DIR_DEFS = ["-DVERSION_US=1", "-DENABLE_RUMBLE=1", "-DF3DEX_GBI=1", "-D_USE_MATH_DEFINES",
               "-DCIMGUI_DEFINE_ENUMS_AND_STRUCTS", "-DNON_MATCHING=1", "-DNON_EQUIVALENT=1", "-DAVOID_UB=1",
               "-DGBI_FLOATS=1", "-DN_MICRO=1", "-DYAML_CPP_STATIC_DEFINE", "-DSTORMLIB_NO_AUTO_LINK"]

# libultraship's own (its src/CMakeLists.txt: target_compile_definitions(libultraship PRIVATE ${GBI_UCODE}))
LUS_DEFS = ["-DF3DEX_GBI_2"]

DEP_INCS = [os.path.join(DEPS, "spdlog", "include"), os.path.join(DEPS, "json", "include"),
            os.path.join(DEPS, "tinyxml2"), os.path.join(DEPS, "zlib"), os.path.join(DEPS, "stb"),
            os.path.join(DEPS, "thread-pool", "include"), os.path.join(DEPS, "monocypher", "src"),
            os.path.join(DEPS, "monocypher", "src", "optional"), os.path.join(DEPS, "dr_libs")]

GEN = os.path.join(BUILD, "gen")

LUS_INCS = [os.path.join(XBOX, "include"), os.path.join(GEN, "lus"), os.path.join(LUS, "include"),
            os.path.join(LUS, "src")] + DEP_INCS

# Dear ImGui, real, for Lighthouse's files (first, ahead of xbox/include's types-only imgui.h, which libultraship's
# files see); its configuration is xbox/include/lhx_imconfig.h
IMGUI = os.path.join(DEPS, "imgui")
IMGUI_DEFS = ['-DIMGUI_USER_CONFIG="lhx_imconfig.h"']

# Lighthouse's CMakeLists.txt include_directories(), in its order (the ones that exist in this libultraship)
# (gen/lh/imcfg first: its imconfig.h is lhx_imconfig.h, for libultraship's headers that include "imconfig.h"
# themselves, where Dear ImGui's own would leave ImTextureID undefined)
LH_INCS = [os.path.join(GEN, "lh", "imcfg"), IMGUI, os.path.join(XBOX, "include"), LH, os.path.join(LH, "include"),
           os.path.join(LH, "lib", "ultralib", "include"), os.path.join(LH, "src"), os.path.join(GEN, "lh"),
           os.path.join(LUS, "include"), os.path.join(LUS, "include", "libultraship"), os.path.join(LUS, "src"),
           # "libultraship/include/..." paths, from the folder libultraship is in
           ROOT,
           # Torch's BK64 headers, with xbox/include/torchx's stand-ins for its factory base and yaml-cpp ahead
           os.path.join(XBOX, "include", "torchx"), os.path.join(TORCH, "src"),
           os.path.join(TORCH, "lib", "binarytools")] + DEP_INCS

LH_DEFS = ["-D_CONSOLE", "-DINCLUDE_GAME_PRINTF", "-DUNICODE", "-D_UNICODE"] + IMGUI_DEFS


# ---------------------------------------------------------------- sources

def rel(p):
    return os.path.relpath(p, ROOT).replace("\\", "/")


def gather(base, patterns, exclude):
    out = []
    for pat in patterns:
        out += glob.glob(os.path.join(base, pat), recursive=True)
    ex = [re.compile(e) for e in exclude]
    return sorted(p for p in set(out) if not any(e.search(rel(p)) for e in ex))


# libultraship: what the Xbox does not have or does not want
LUS_EXCLUDE = [
    r"/fast/backends/gfx_(sdl2|opengl|dxgi|direct3d11|direct3d_common|metal|metal_shader)\.cpp$",
    r"/fast/Fast3dGui\.cpp$",
    r"/ship/window/gui/",                       # ImGui
    r"/libultraship/window/gui/",               # ImGui: gfx debugger, input editor
    r"/physicaldevice/SDLAddRemoveDeviceEventHandler\.cpp$",  # a GuiWindow polling SDL events
    r"/audio/(SDL|Wasapi|CoreAudio)AudioPlayer\.cpp$",
    r"/ship/port/mobile/",
    r"/ship/scripting/", r"/ship/security/",    # tcc scripting and its keys
    r"\.mm$", r"/utils/AppleFolderManager",
    r"/ship/resource/archive/OtrArchive\.cpp$",  # MPQ
]

# Lighthouse's CMake leaves out src/unused; then what the Xbox leaves out
LH_EXCLUDE = [
    r"^Lighthouse/src/unused/",
    r"^Lighthouse/src/port/build\.c$",          # written by the PC CMake configure; build/gen/lh/build.c is ours
    r"^Lighthouse/src/port/Network/",           # online play (SDL2_net)
    r"^Lighthouse/src/port/Extractor/",         # the archives are made on a PC (setup.py, Torch)
    r"^Lighthouse/src/port/Controller/Mapper\.cpp$",  # raw (DirectInput) joysticks: every Xbox pad is a controller
    # the PC's mod and romhack manager (installs .o2r mods, extracts romhacks with Torch in the game): the Xbox plays
    # the archives setup.py made; what the engine asks of it is in xbox/src/port/lhx_nomods.cpp
    r"^Lighthouse/src/port/UI/LighthouseModMenuWindow\.cpp$",
]

# Lighthouse's GUI: its menus, windows, editors and overlays, all Dear ImGui (built for its headers' sake, never
# drawn). The Xbox's menu is xbox/src/lusx_menu.cpp, on Back; what the game needs from these files is in
# xbox/src/port/lhx_nogui.cpp.
LH_GUI_EXCLUDE = [
    r"^Lighthouse/src/port/UI/",
    r"^Lighthouse/src/port/DevTools/(EventDebugger|OcclusionDebug)\.cpp$",
    # (the randomizer's menu, tracker and seed-metrics windows; its logic stays, with the reachability tables the
    # tracker's file holds, in lhx_nogui.cpp)
    r"^Lighthouse/src/port/Rando/(CheckTracker/CheckTracker|LighthouseMenuRando|Logic/Metrics)\.cpp$",
    r"^Lighthouse/src/port/Enhancements/(Cosmetics/CosmeticsEditor|Trackers/DisplayOverlay|"
    r"Trackers/WorldTracker/WorldTracker)\.cpp$",
    r"^Lighthouse/src/port/Presets/Presets\.cpp$",  # the presets window
]


# (no xbox_save.cpp yet: Lighthouse keeps its saves in T:\saves, its SaveManager in libultraship's app directory; the
# other Lus-X ports' save games on the dashboard, one a slot, are still to come)
XBOX_PENDING = r"^xbox/src/xbox_save\.cpp$"


def groups():
    stb_impl = os.path.join(GEN, "stb_impl.c")
    g = []
    g.append(dict(
        name="deps", incs=DEP_INCS, defs=COMMON_DEFS, extra=["-w", "-ffreestanding", "-fno-builtin"],
        srcs=[os.path.join(DEPS, "spdlog", "src", n + ".cpp") for n in
              ("spdlog", "stdout_sinks", "color_sinks", "file_sinks", "async", "cfg", "bundled_fmtlib_format")] +
             [os.path.join(DEPS, "tinyxml2", "tinyxml2.cpp")] +
             [os.path.join(DEPS, "zlib", n + ".c") for n in
              ("adler32", "crc32", "inflate", "inffast", "inftrees", "zutil")] +
             [os.path.join(DEPS, "monocypher", "src", "monocypher.c"),
              os.path.join(DEPS, "monocypher", "src", "optional", "monocypher-ed25519.c"), stb_impl]))
    g.append(dict(
        # Dear ImGui's core, for Lighthouse's calls into it (never drawn: lhx_imconfig.h); cold, so -Os
        name="imgui", incs=[IMGUI, os.path.join(XBOX, "include")], defs=COMMON_DEFS + IMGUI_DEFS,
        extra=["-w", "-Os"],
        srcs=[os.path.join(IMGUI, n + ".cpp") for n in ("imgui", "imgui_draw", "imgui_tables", "imgui_widgets")]))
    g.append(dict(
        name="lus", incs=LUS_INCS, defs=COMMON_DEFS + LH_DIR_DEFS + LUS_DEFS + cvar_defines(),
        extra=["-Wno-everything"],
        # the display-list interpreter and the NV2A backend are the hottest CPU code: -O3
        hot=[r"/fast/interpreter\.cpp$", r"/fast/backends/gfx_direct3d8\.cpp$"], hot_opt="-O3",
        srcs=gather(os.path.join(LUS, "src"), ["**/*.cpp", "**/*.c"], LUS_EXCLUDE)))
    g.append(dict(
        name="game", incs=LH_INCS, defs=COMMON_DEFS + LH_DIR_DEFS + cvar_defines() + LH_DEFS,
        # as Lighthouse builds on an x86 PC: GCC bitfields; and as its CMake, the decomp's
        # pointer/integer mixing allowed (errors by default in this clang)
        extra=["-w", "-fno-strict-aliasing", "-fwrapv", "-mno-ms-bitfields",
               "-Wno-error=incompatible-pointer-types", "-Wno-error=int-conversion",
               "-Wno-error=incompatible-function-pointer-types", "-Wno-error=implicit-function-declaration",
               "-Wno-error=implicit-int", "-Wno-error=return-type"],
        srcs=gather(LH, ["src/**/*.c", "lib/ultralib/**/*.c"], LH_EXCLUDE)))
    g.append(dict(
        name="port", incs=LH_INCS, defs=COMMON_DEFS + LH_DIR_DEFS + cvar_defines() + LH_DEFS,
        # -Os: Lighthouse's C++ is mostly cold (enhancement hooks, the randomizer, resource loaders); what runs
        # every frame stays -O2 (hot)
        extra=["-w", "-fno-strict-aliasing", "-mno-ms-bitfields", "-Os"],
        hot=[r"/src/port/Interpolation/", r"/src/port/OS/", r"/src/port/Audio/", r"/src/port/Engine\.cpp$",
             r"/src/port/Game\.cpp$", r"/src/port/Enhancements/Events/"],
        # with xbox/src/port: what the game needs from the files left out; and libultraship's GUI window base classes,
        # which Lighthouse's own windows derive from (built here, against the real ImGui: libultraship's group has none)
        srcs=gather(LH, ["src/**/*.cpp"], LH_EXCLUDE + LH_GUI_EXCLUDE) + gather(XBOX, ["src/port/**/*.cpp"], []) +
             [os.path.join(LUS, "src", "ship", "window", "gui", n + ".cpp") for n in ("GuiElement", "GuiWindow")]))
    g.append(dict(
        # (as libultraship's: xbox/include's imgui.h, not the real one)
        # (and Lighthouse's src/, for the menu's ShipInit.hpp)
        name="xbox", incs=[os.path.join(XBOX, "src")] + LUS_INCS + [os.path.join(LH, "src")],
        defs=COMMON_DEFS + LH_DIR_DEFS + cvar_defines(),
        extra=["-Wno-everything", "-fwrapv", "-fno-strict-aliasing"],
        srcs=gather(XBOX, ["src/**/*.cpp", "src/**/*.c"], [r"^xbox/src/port/", XBOX_PENDING])))
    return g


# ---------------------------------------------------------------- generated

def write_if_changed(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    old = None
    if os.path.isfile(path):
        with open(path, encoding="utf-8", newline="") as fh:
            old = fh.read()
    if old != text:
        with open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write(text)


def lh_version():
    with open(os.path.join(LH, "CMakeLists.txt"), encoding="utf-8") as fh:
        m = re.search(r"project\(Lighthouse VERSION (\d+)\.(\d+)\.(\d+)", fh.read())
    return m.group(1), m.group(2), m.group(3)


NATO = ("Alfa Bravo Charlie Delta Echo Foxtrot Golf Hotel India Juliett Kilo Lima Mike November Oscar Papa Quebec "
        "Romeo Sierra Tango Uniform Victor Whiskey Xray Yankee Zulu").split()


def flag_names_header():
    """include/enums.h's flag enums as name tables, as cmake/GenerateFlagNames.cmake makes them."""
    with open(os.path.join(LH, "include", "enums.h"), encoding="utf-8", errors="replace") as fh:
        lines = fh.read().splitlines()

    def parse(enum):
        rows, inside, nxt = [], False, 0
        for line in lines:
            if not inside:
                if re.match(r"^[ \t]*enum[ \t]+%s[ \t]*(\{)?[ \t]*$" % re.escape(enum), line):
                    inside = True
                continue
            if re.match(r"^[ \t]*\}", line):
                break
            clean = re.sub(r"/\*.*\*/", "", re.sub(r"//.*$", "", line))
            m = re.match(r"^[ \t]*([A-Za-z_][A-Za-z0-9_]*)[ \t]*(=[ \t]*(0[xX][0-9a-fA-F]+|[0-9]+))?[ \t]*,?[ \t]*$", clean)
            if m:
                if m.group(3):
                    nxt = int(m.group(3), 0)
                rows.append('    { %d, "%s" },' % (nxt, m.group(1)))
                nxt += 1
        if not rows:
            raise SystemExit("build: no entries for enum %s in include/enums.h" % enum)
        return "\n".join(rows)

    def table(enum, name):
        return "// %s\nstatic const FlagName %s[] = {\n%s\n};\n" % (enum, name, parse(enum))

    body = ("// GENERATED at configure time by cmake/GenerateFlagNames.cmake from include/enums.h.\n#pragma once\n\n"
            "struct FlagName {\n    int index;\n    const char* name;\n};\n")
    for enum, name in (("file_progress_e", "kFileProgressNames"), ("volatile_flags_e", "kVolatileNames"),
                       ("level_flags_e", "kLevelNames")):
        body += "\n" + table(enum, name)
    groups_ = [("sm_specific_flags", "kSmNames", "0xB"), ("mm_specific_flags", "kMmNames", "0x1"),
               ("ttc_specific_flags", "kTtcNames", "0x2"), ("bgs_specific_flags", "kBgsNames", "0x4"),
               ("fp_specific_flags", "kFpNames", "0x5"), ("mmm_specific_flags", "kMmmNames", "0xA"),
               ("rbb_main_specific_flags", "kRbbMainNames", "0x9"),
               ("rbb_boat_room_specific_flags", "kRbbBoatNames", "0x9"),
               ("ccw_zubba_specific_flags", "kCcwZubbaNames", "0x8"),
               ("ccw_winter_specific_flags", "kCcwWinterNames", "0x8"), ("lair_specific_flags", "kLairNames", "0x6")]
    rows = []
    for enum, name, level in groups_:
        body += "\n" + table(enum, name)
        rows.append("    { %s, %s, (int)(sizeof(%s) / sizeof(%s[0])) }," % (level, name, name, name))
    body += ("\n// Map-specific lookup needs the loaded level to disambiguate.\nstruct MapFlagGroup {\n    int level;\n"
             "    const FlagName* names;\n    int count;\n};\n\nstatic const MapFlagGroup kMapFlagGroups[] = {\n%s\n};\n"
             % "\n".join(rows))
    return body


def generate():
    # a header and not a -D, so switching rebuilds only the files that include it: a -D is in every command. 1: the test
    # tools are in every build, off until the Debug page turns them on; 0 here would leave them out
    for inc in ("lus", "lh"):
        write_if_changed(os.path.join(GEN, inc, "lusx_debug_config.h"), "#define LUSX_DEBUG_TOOLS 1\n")
    write_if_changed(os.path.join(GEN, "stb_impl.c"),
                     "#define STB_IMAGE_IMPLEMENTATION\n#define STBI_NO_THREAD_LOCALS\n#define STBI_NO_SIMD\n#include \"stb_image.h\"\n")
    write_if_changed(os.path.join(GEN, "lus", "ship", "install_config.h"),
                     '#define CMAKE_INSTALL_PREFIX "D:\\\\"\n#define DISABLE_DLL_LOADER\n')
    major, minor, patch = lh_version()
    subs = {"PROJECT_BUILD_NAME": "Montauk " + NATO[int(patch)], "PROJECT_TEAM": "github.com/harbourmasters",
            "PROJECT_NAME": "Lighthouse",
            "CMAKE_PROJECT_VERSION_MAJOR": major, "CMAKE_PROJECT_VERSION_MINOR": minor,
            "CMAKE_PROJECT_VERSION_PATCH": patch, "CMAKE_PROJECT_GIT_BRANCH": TITLE,
            "CMAKE_PROJECT_GIT_COMMIT_HASH": LH_COMMIT, "CMAKE_PROJECT_GIT_COMMIT_TAG": "", "GIT_CONTRIBUTORS": ""}
    # src/port/build.c and res/properties.h, from their .in files (Lighthouse's CMake writes them into its build tree)
    for src, dst in (("src/port/build.c.in", "build.c"), ("res/properties.h.in", "properties.h")):
        with open(os.path.join(LH, *src.split("/")), encoding="utf-8") as fh:
            text = fh.read()
        write_if_changed(os.path.join(GEN, "lh", dst), re.sub(r"@(\w+)@", lambda m: subs.get(m.group(1), ""), text))
    write_if_changed(os.path.join(GEN, "lh", "FlagNames.generated.h"), flag_names_header())
    write_if_changed(os.path.join(GEN, "lh", "imcfg", "imconfig.h"), '#pragma once\n#include "lhx_imconfig.h"\n')
    write_if_changed(os.path.join(GEN, "lh", "bk64_torch.cpp"), torch_map_reader())


def torch_map_reader():
    """Torch's BK64::ReadMapResource, the one function of Torch Lighthouse's game side calls (its BKMap resource
    importer, src/port/Resource/Importers/MapFactory.cpp): taken from the pinned Torch's MapFactory.cpp as it is, so
    it never differs from the extractor that wrote the maps. The rest of that file is Torch's extraction side."""
    path = os.path.join(TORCH, "src", "factories", "bk64", "MapFactory.cpp")
    if not os.path.isfile(path):
        raise SystemExit("build: no %s -- run setup.py first (it fetches Torch)" % rel(path))
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    m = re.search(r"^std::shared_ptr<MapData> ReadMapResource\(const std::vector<uint8_t>& resource\) \{\n.*?^\}\n",
                  text, re.S | re.M)
    if m is None:
        raise SystemExit("build: ReadMapResource not found in %s" % rel(path))
    return ("// Generated by tools/build.py: BK64::ReadMapResource from Torch's src/factories/bk64/MapFactory.cpp\n"
            "// (build/upstream/torch, MIT), unchanged.\n"
            "#include \"factories/bk64/MapFactory.h\"\n#include \"factories/bk64/BKResource.h\"\n\n"
            "#include <cstring>\n#include <spdlog/spdlog.h>\n\nnamespace BK64 {\n\n" + m.group(0) + "\n} // namespace BK64\n")


def extra_sources():
    return {"game": [os.path.join(GEN, "lh", "build.c")], "port": [os.path.join(GEN, "lh", "bk64_torch.cpp")]}


# ---------------------------------------------------------------- compiling

# Files built native, never as LTO bitcode: each defines a C function `f` and another `_f` (the decomp's names). On
# 32-bit COFF a C symbol is its name with an underscore before it, so `f`'s symbol is `_f`, the other's IR name, and
# lld's ThinLTO then loses `f` ("undefined symbol: _f" where it's defined). Native objects name them apart.
NO_LTO = [r"^Lighthouse/src/core2/bs/beeFly\.c$", r"^Lighthouse/src/core2/gc/bound\.c$"]


def is_lto(group, src):
    return LTO and group in LTO_GROUPS and not any(re.search(p, rel(src)) for p in NO_LTO)


def obj_path(group, src):
    r = os.path.relpath(src, ROOT)
    if r.startswith(".."):
        r = os.path.basename(src)
    # bitcode and native objects apart, so a --no-lto build never recompiles the other's
    obj = "obj-lto" if is_lto(group, src) else "obj"
    return os.path.join(BUILD, obj, group, os.path.splitext(r)[0] + ".obj")


def lang_of(src):
    return "c++" if src.endswith((".cpp", ".cc", ".cxx")) else "c"


def compile_cmd(g, src, obj):
    lang = lang_of(src)
    cmd = [CLANG] + rxdk_flags(lang) + g["extra"] + g["defs"]
    for inc in g["incs"]:
        cmd += ["-I", inc]
    cmd += ["-I", SDK_INC]
    hot = any(re.search(h, rel(src)) for h in g.get("hot", []))
    if hot:
        cmd += [g.get("hot_opt", "-O2")]
    if is_lto(g["name"], src):
        cmd += ["-flto" if g["name"] in FULL_LTO_GROUPS else "-flto=thin"]
    cmd += ["-x", lang, "-c", src, "-o", obj, "-MD", "-MF", obj + ".d"]
    return cmd


def deps_newer(obj, cmd_text):
    d = obj + ".d"
    c = obj + ".cmd"
    if not (os.path.isfile(obj) and os.path.isfile(d) and os.path.isfile(c)):
        return True
    with open(c, encoding="utf-8") as fh:
        if fh.read() != cmd_text:
            return True
    t = os.path.getmtime(obj)
    with open(d, encoding="utf-8", errors="replace") as fh:
        text = fh.read().replace("\\\n", " ")
    m = re.match(r"^.*?\.obj:\s", text, re.S)
    body = text[m.end():] if m else text
    for dep in body.split():
        dep = dep.replace("\\ ", " ")
        try:
            if os.path.getmtime(dep) > t:
                return True
        except OSError:
            return True
    return False


def run(cmd, log):
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    out = (r.stdout or "") + (r.stderr or "")
    if r.returncode != 0:
        os.makedirs(os.path.dirname(log), exist_ok=True)
        with open(log, "w", encoding="utf-8") as fh:
            fh.write(" ".join(cmd) + "\n\n" + out)
    return r.returncode, out


def first_errors(text, n=3):
    lines = [l for l in text.splitlines() if " error: " in l or l.startswith("error:") or "fatal error" in l]
    return "\n".join(lines[:n]) if lines else "\n".join(text.splitlines()[-5:])


def compile_group(g, jobs, keep_going, file_filter, show_cmd):
    srcs = g["srcs"] + extra_sources().get(g["name"], [])
    if file_filter:
        srcs = [s for s in srcs if re.search(file_filter, rel(s))]
    todo, objs = [], []
    for s in srcs:
        o = obj_path(g["name"], s)
        cmd = compile_cmd(g, s, o)
        if show_cmd and re.search(show_cmd, rel(s)):
            print(subprocess.list2cmdline(cmd))
        objs.append(o)
        text = "\n".join(cmd)
        if deps_newer(o, text):
            todo.append((s, o, cmd, text))
    failed = []
    if not todo:
        return objs, failed
    print("build: %s: %d of %d to compile" % (g["name"], len(todo), len(srcs)), flush=True)
    t0 = time.time()
    done = 0

    def one(item):
        s, o, cmd, text = item
        os.makedirs(os.path.dirname(o), exist_ok=True)
        log = os.path.join(BUILD, "log", g["name"], os.path.splitext(rel(s))[0].replace("/", "_") + ".log")
        rc, out = run(cmd, log)
        if rc == 0:
            with open(o + ".cmd", "w", encoding="utf-8") as fh:
                fh.write(text)
            if os.path.isfile(log):
                os.remove(log)
        return s, rc, out

    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        futs = [ex.submit(one, it) for it in todo]
        for f in concurrent.futures.as_completed(futs):
            s, rc, out = f.result()
            done += 1
            if rc != 0:
                failed.append(s)
                print("  FAIL %s\n%s" % (rel(s), "\n".join("       " + l for l in first_errors(out).splitlines())),
                      flush=True)
                if not keep_going:
                    for x in futs:
                        x.cancel()
                    break
            if done % 100 == 0:
                print("  %d/%d (%.0f s)" % (done, len(todo), time.time() - t0), flush=True)
    print("build: %s: %d compiled, %d failed (%.0f s)" % (g["name"], done - len(failed), len(failed),
                                                           time.time() - t0), flush=True)
    return objs, failed


# ---------------------------------------------------------------- linking

EH_BEGIN = '.section .eh_frame,"dr"\n.globl ___eh_frame_start\n___eh_frame_start:\n'
EH_END = ('.section .eh_frame$z,"dr"\n.globl ___eh_frame_end\n.globl ___rxdk_eh_frame_end_requires_rxdk_tools_1_3_6\n'
          '___eh_frame_end:\n___rxdk_eh_frame_end_requires_rxdk_tools_1_3_6:\n')


def archive(name, objs):
    """deps, imgui and libultraship as static libraries, as on the PC: an object is linked only when something
    needs it."""
    lib = os.path.join(BUILD, "lib", name + ("-lto" if LTO and name in LTO_GROUPS else "") + ".lib")
    os.makedirs(os.path.dirname(lib), exist_ok=True)
    if os.path.isfile(lib) and all(os.path.getmtime(o) <= os.path.getmtime(lib) for o in objs):
        return lib
    if os.path.isfile(lib):
        os.remove(lib)
    rsp = lib + ".rsp"
    with open(rsp, "w", encoding="utf-8") as fh:
        fh.write("\n".join('"%s"' % o.replace("\\", "/") for o in objs) + "\n")
    r = subprocess.run([os.path.join(LLVM, "bin", "llvm-ar.exe"), "rcs", lib, "@" + rsp])
    if r.returncode != 0:
        raise SystemExit("build: archiving %s failed" % name)
    return lib


def link(objs):
    lk = os.path.join(BUILD, "link")
    os.makedirs(lk, exist_ok=True)
    eh = []
    for name, text in (("rxdk_eh_begin", EH_BEGIN), ("rxdk_eh_end", EH_END)):
        s = os.path.join(lk, name + ".S")
        write_if_changed(s, text)
        o = os.path.join(lk, name + ".o")
        r = subprocess.run([CLANG, "-target", "i686-pc-windows-gnu", "-march=pentium3", "-mlong-double-64", "-c", s,
                            "-o", o])
        if r.returncode != 0:
            raise SystemExit("build: assembling %s failed" % s)
        eh.append(o)
    rsp = os.path.join(lk, "objs.rsp")
    with open(rsp, "w", encoding="utf-8") as fh:
        fh.write("\n".join('"%s"' % o.replace("\\", "/") for o in objs) + "\n")
    exe = os.path.join(BUILD, TITLE + ".exe")
    cmd = [CLANG, eh[0], "@" + rsp] + [os.path.join(SDK_LIB, l) for l in LIBS] + [builtins_lib(), eh[1],
           "-target", "i686-pc-windows-gnu", "-march=pentium3", "-nostdlib", "-nostartfiles",
           "-Wl,--image-base=0x10000", "-Wl,--gc-sections", "-Wl,--error-limit=0",
           "-Wl,-Map=" + os.path.join(lk, TITLE + ".map"),
           # xbox_glue.cpp's strlen, which names the caller of a strlen on a bad pointer
           "-Wl,--wrap=strlen",
           "-fuse-ld=lld", "-e", "XapiTitleStartup", "-o", exe]
    if LTO:
        # -emulated-tls as the compiles' -femulated-tls: it's a code generator's option, not kept in the bitcode, and
        # without it LTO quietly makes native TLS (__tls_index). The cache: a file's change recompiles that module.
        cmd += ["-Wl,-mllvm,-emulated-tls", "-Wl,-mllvm,-import-instr-limit=%d" % LTO_IMPORT_LIMIT,
                "-Wl,--thinlto-jobs=%d" % (os.cpu_count() or 4),
                "-Wl,--thinlto-cache-dir=" + os.path.join(BUILD, "lto-cache").replace("\\", "/")]
    print("build: linking %d objects" % len(objs), flush=True)
    rc, out = run(cmd, os.path.join(BUILD, "log", "link.log"))
    if rc != 0:
        undef = sorted(set(re.findall(r"undefined symbol: (.+)", out)))
        print("build: link failed: %d undefined symbols (build/log/link.log)" % len(undef))
        for u in undef[:80]:
            print("  " + u)
        if not undef:
            print(first_errors(out, 20))
        raise SystemExit(1)
    xbe = os.path.join(BUILD, TITLE + ".xbe")
    # the dashboard's title and save images (tools/make_icons.py)
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import make_icons
    icons = make_icons.main()
    images = ["/TITLEIMAGE:" + icons[0], "/DEFAULTSAVEIMAGE:" + icons[1]] if icons else []
    r = subprocess.run([os.path.join(RXDK, "tools", "imagebld.exe"), "/in:" + exe, "/out:" + xbe, "/nologo",
                        "/stack:%d" % STACK, "/nolibwarn", "/TESTID:" + TITLE_ID, "/TESTNAME:" + TITLE_NAME,
                        "/TESTVERSION:1"] + images, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stdout.write(r.stdout + r.stderr)
        raise SystemExit("build: imagebld failed")
    out = os.path.join(ROOT, "out", "Release")
    stage = os.path.join(out, "Build", TITLE)
    if os.path.isdir(stage):
        shutil.rmtree(stage)
    os.makedirs(stage)
    shutil.copy(xbe, os.path.join(stage, "default.xbe"))
    shutil.copy(os.path.join(XBOX, "assets", "menu.ttf"), stage)  # the port's menu reads it when it opens
    data = os.path.join(ROOT, "data")
    archives = sorted(glob.glob(os.path.join(data, "*.o2r")))
    for a in archives:
        shutil.copy(a, stage)
    os.makedirs(os.path.join(out, "XISO"), exist_ok=True)
    iso = os.path.join(out, "XISO", TITLE + ".iso")
    r = subprocess.run([os.path.join(RXDK, "tools", "xdvdfs.exe"), "pack", stage, iso], capture_output=True,
                       text=True)
    if r.returncode != 0:
        sys.stdout.write(r.stdout + r.stderr)
        raise SystemExit("build: xdvdfs failed")
    print("build: %s (%.2f MB), %s (%.1f MB; archives: %s)" % (
        rel(xbe), os.path.getsize(xbe) / 1048576.0, rel(iso), os.path.getsize(iso) / 1048576.0,
        ", ".join(os.path.basename(a) for a in archives) or
        "none -- run setup.py: it makes bk.o2r and lighthouse.o2r in data/"))
    size = subprocess.run([os.path.join(LLVM, "bin", "llvm-size.exe"), "-A", exe], capture_output=True, text=True)
    sys.stdout.write(size.stdout)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-k", "--keep-going", action="store_true")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--only", help="comma-separated groups to compile (no link)")
    ap.add_argument("--file", help="compile only sources whose path matches this regex (no link)")
    ap.add_argument("--cmd", help="print the compile command of sources matching this regex")
    ap.add_argument("--no-link", action="store_true")
    ap.add_argument("--clean", action="store_true")
    ap.add_argument("--no-lto", action="store_true", help="native objects: a quicker link, or to compare")
    a = ap.parse_args()
    global LTO
    LTO = not a.no_lto
    if a.clean and os.path.isdir(BUILD):
        for sub in ("obj", "obj-lto", "lib", "link", "log", "gen", "lto-cache"):
            shutil.rmtree(os.path.join(BUILD, sub), ignore_errors=True)
    generate()
    # its "built" line is __DATE__ and __TIME__: compiled every time, so a log says which build it's from
    os.utime(os.path.join(XBOX, "src", "xbox_main.cpp"))
    only = set(a.only.split(",")) if a.only else None
    all_objs, all_failed, libs = [], [], []
    for g in groups():
        if only and g["name"] not in only:
            continue
        objs, failed = compile_group(g, a.jobs, a.keep_going, a.file, a.cmd)
        if g["name"] in ("deps", "imgui", "lus") and not failed and not (a.no_link or only or a.file):
            libs.append(archive(g["name"], objs))
        else:
            all_objs += objs
        all_failed += failed
        if failed and not a.keep_going:
            break
    if all_failed:
        print("build: %d files failed (logs in build/log)" % len(all_failed))
        raise SystemExit(1)
    if not (a.no_link or only or a.file):
        # the game's objects, then libultraship and its dependencies as libraries
        link(all_objs + libs)


if __name__ == "__main__":
    main()
