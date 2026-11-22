// lhx_nomods.cpp -- Banjo-Kazooie-X: what Lighthouse's engine asks of the parts the Xbox leaves out.
//
// The mod and romhack manager (src/port/UI/LighthouseModMenuWindow.cpp): on a PC it scans mods/ for .o2r archives,
// enables and orders them, and extracts romhacks and language packs from a ROM with Torch inside the game. The Xbox
// plays the two archives setup.py made beside default.xbe (bk.o2r, lighthouse.o2r) and the archives in mods/ beside
// it (UpdateModFiles: texture packs made for the Xbox by tools/hd_pack.py), each off until the Back menu's Mods page
// turns it on, in the order it set; no romhack is active and nothing is extracted.
//
// The extractor (src/port/Extractor/ExtractFlow.cpp): GameEngine::RunExtract finds or makes bk.o2r at start-up;
// setup.py has made it.
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <libultraship/libultraship.h>
#include <spdlog/spdlog.h>

#include "port/Engine.h"
#include "port/UI/LighthouseModMenuWindow.h"

#include "lusx_options.h"

// the Back menu's Mods page: the ones found, in their order (the top one wins), the ones it has turned on and the
// order it has set ("a.o2r|b.zip", names, case aside). A mod is off until it's turned on: the game as it was.
static std::vector<LusxMod> sMods;
static const char* const kModsOn = "gLusx.ModsOn";
static const char* const kModsOrder = "gLusx.ModsOrder";

static bool SameName(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
               return toupper(x) == toupper(y);
           });
}

static std::vector<std::string> Names(const char* cvar) {
    const std::string all = CVarGetString(cvar, "");
    std::vector<std::string> files;
    size_t at = 0;
    while (at < all.size()) {
        size_t bar = all.find('|', at);
        bar = bar == std::string::npos ? all.size() : bar;
        if (bar > at) {
            files.push_back(all.substr(at, bar - at));
        }
        at = bar + 1;
    }
    return files;
}

const std::vector<LusxMod>& LusxMods_List(void) {
    return sMods;
}

bool LusxMods_IsOff(const std::string& file) {
    for (const std::string& f : Names(kModsOn)) {
        if (SameName(f, file)) {
            return false;
        }
    }
    return true;
}

void LusxMods_Move(int index, int by) {
    const int to = index + by;
    if (index < 0 || to < 0 || index >= (int)sMods.size() || to >= (int)sMods.size()) {
        return;
    }
    std::swap(sMods[index], sMods[to]);
    // the whole order kept: the ones found now, top first
    std::string all;
    for (const LusxMod& mod : sMods) {
        all += (all.empty() ? "" : "|") + mod.file;
    }
    CVarSetString(kModsOrder, all.c_str());
}

void LusxMods_SetOff(const std::string& file, bool off) {
    std::string all;
    for (const std::string& f : Names(kModsOn)) {
        if (!SameName(f, file)) {
            all += (all.empty() ? "" : "|") + f;
        }
    }
    if (!off) {
        all += (all.empty() ? "" : "|") + file;
    }
    CVarSetString(kModsOn, all.c_str());
}

// at start-up: mods/ beside default.xbe (D:, the disc or the folder it was launched from), its .o2r and .zip
// archives added after the game's own, the ones the Mods page turned on. Their order is the Mods page's,
// the top one added last so that it wins where two change the same thing; ones it hasn't ordered yet (new to the
// folder, or all of them the first time) go on top, the last by name highest, as a PC's name order has it (a later
// one wins). A pack's textures under alt/ take the game's place while Lighthouse's alternate assets are on (Options,
// Display and sound, HD textures: off by default on the Xbox, Engine.cpp)
void UpdateModFiles(bool init, bool reset) {
    (void)reset;
    if (!init) {
        return;
    }
    const std::string dir = Ship::Context::GetPathRelativeToAppBundle("mods");
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return;
    }
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        std::string name = entry.path().filename().generic_string();
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)tolower(c); });
        if (entry.is_regular_file(ec) && (lower.ends_with(".o2r") || lower.ends_with(".zip"))) {
            names.push_back(name);
        }
    }
    // by name, the last first
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        return std::lexicographical_compare(b.begin(), b.end(), a.begin(), a.end(),
                                            [](unsigned char x, unsigned char y) { return toupper(x) < toupper(y); });
    });
    // the ordered ones after them, in the order kept
    std::vector<std::string> order;
    std::vector<bool> placed(names.size(), false);
    const std::vector<std::string> kept = Names(kModsOrder);
    for (size_t i = 0; i < names.size(); i++) {
        const bool known = std::any_of(kept.begin(), kept.end(), [&](const std::string& k) { return SameName(k, names[i]); });
        if (!known) {
            order.push_back(names[i]);
            placed[i] = true;
        }
    }
    for (const std::string& k : kept) {
        for (size_t i = 0; i < names.size(); i++) {
            if (!placed[i] && SameName(k, names[i])) {
                order.push_back(names[i]);
                placed[i] = true;
            }
        }
    }
    sMods.clear();
    for (const std::string& name : order) {
        sMods.push_back({ name, !LusxMods_IsOff(name), (int)sMods.size() });
    }
    // added from the bottom up: the top one last
    for (size_t i = sMods.size(); i-- > 0;) {
        const std::string path = dir + "/" + sMods[i].file;
        if (!sMods[i].on) {
            SPDLOG_INFO("lus-x: mod, off (Back menu, Mods, to turn it on): {}", path);
            continue;
        }
        SPDLOG_INFO("lus-x: mod {} of {}: {}", i + 1, sMods.size(), path);
        Ship::Context::GetRawInstance()->GetResourceManager()->GetArchiveManager()->AddArchive(path);
    }
}

bool IsScopedModFolderName(const std::string& topLevelName) {
    (void)topLevelName;
    return false;
}

std::string GetActiveRomhackBasename() {
    return "";
}

void EnableMod(std::string file) {
    (void)file;
}

void DisableMod(std::string file) {
    (void)file;
}

void MaybeShowModConflictPopup() {
}

void NoteOutdatedLanguagePack(const std::string& name) {
    (void)name;
}

void MaybeShowOutdatedArchivesPopup() {
}

void SetSoleEnabledRomhack(const std::string& keepBasename) {
    (void)keepBasename;
}

void RequestInlineModExtraction() {
}

void RequestInlineLanguagePackExtraction() {
}

void DrawInlineModExtraction() {
}

bool IsInlineModExtractionBusy() {
    return false;
}

// (the windows the PC menu opens them in: never drawn here)
void LighthouseModMenuWindow::DrawElement() {
}

void LighthouseRomhackMenuWindow::DrawElement() {
}

void GameEngine::RunExtract(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
}
