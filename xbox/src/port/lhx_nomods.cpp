// lhx_nomods.cpp -- Banjo-Kazooie-X: what Lighthouse's engine asks of the parts the Xbox leaves out.
//
// The mod and romhack manager (src/port/UI/LighthouseModMenuWindow.cpp): on a PC it scans mods/ for .o2r archives,
// enables and orders them, and extracts romhacks and language packs from a ROM with Torch inside the game. The Xbox
// plays the two archives setup.py made beside default.xbe (bk.o2r, lighthouse.o2r), so there is no mod to scan, none
// enabled, no romhack active and nothing being extracted.
//
// The extractor (src/port/Extractor/ExtractFlow.cpp): GameEngine::RunExtract finds or makes bk.o2r at start-up;
// setup.py has made it.
#include <string>

#include "port/Engine.h"
#include "port/UI/LighthouseModMenuWindow.h"

void UpdateModFiles(bool init, bool reset) {
    (void)init;
    (void)reset;
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
