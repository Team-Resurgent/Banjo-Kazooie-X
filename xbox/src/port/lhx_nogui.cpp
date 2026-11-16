// lhx_nogui.cpp -- Banjo-Kazooie-X: what the game asks of Lighthouse's GUI, which the Xbox leaves out
// (tools/build.py, LH_GUI_EXCLUDE: its ImGui menus, windows, editors and overlays).
//
// The Xbox's own menu is on Back (xbox/src/lusx_menu.cpp), drawn by the port's overlay, not ImGui. Here:
//   - Lighthouse's menu set-up and draw calls: nothing to set up, nothing drawn
//   - its pop-ups and notifications: a log line each, as a pop-up's first button would answer
//   - the GUI textures it loads for ImGui (button icons): none
//   - the aiming crosshair (an ImGui drawing): not drawn; the aim still works as in the N64 game
//   - the randomizer's check tracker and seed-metrics windows: none (the randomizer's game side stays, and with it the
//     reachability tables its logic fills, which the tracker's file holds: CheckTracker/CheckTracker.cpp)
#include <functional>
#include <string>

#include <spdlog/spdlog.h>

#include "fast/Fast3dGui.h"
#include "port/UI/Crosshair.h"
#include "port/UI/LighthouseGui.hpp"
#include "port/UI/Notification.h"
#include "port/Rando/CheckTracker/CheckTracker.h"
#include "port/Rando/Logic/Logic.h"

Rando::StaticData::RandoLogicData reachableRegions[RR_MAX];
Rando::StaticData::RandoLogicData reachableEvents[RA_MAX];
Rando::StaticData::RandoLogicData reachableChecks[RC_MAX];

void DrawSeedMetrics() {
}

void RefreshMetrics(std::string text) {
    (void)text;
}

namespace LighthouseGui {

void SetupMenu() {
}

void SetupGuiElements() {
}

void Draw() {
}

void Destroy() {
}

void SetAttached(bool attached) {
    (void)attached;
}

void RegisterPopup(std::string title, std::string message, std::string button1, std::string button2,
                   std::function<void()> button1callback, std::function<void()> button2callback) {
    (void)button2;
    (void)button2callback;
    SPDLOG_INFO("{}: {} ({})", title, message, button1);
    if (button1callback) {
        button1callback();
    }
}

size_t PopupsQueued() {
    return 0;
}

UIWidgets::Colors GetMenuThemeColor() {
    return UIWidgets::Colors::Red;
}

} // namespace LighthouseGui

namespace Notification {

void Emit(Options notification) {
    SPDLOG_INFO("{}{}{}", notification.prefix, notification.message, notification.suffix);
}

} // namespace Notification

namespace Crosshair {

void ShowThisFrame() {
}

} // namespace Crosshair

namespace Rando::CheckTracker {

void Init() {
}

void LoadFromPreset(const nlohmann::json& info) {
    (void)info;
}

} // namespace Rando::CheckTracker

namespace Fast {

void Fast3dGui::LoadGuiTexture(const std::string& name, const std::string& path, const std::string& palettePath,
                               const ImVec4& tint) {
    (void)name;
    (void)path;
    (void)palettePath;
    (void)tint;
}

void Fast3dGui::LoadGuiTexture(const std::string& name, const Fast::Texture& tex, const std::string& palettePath,
                               const ImVec4& tint) {
    (void)name;
    (void)tex;
    (void)palettePath;
    (void)tint;
}

} // namespace Fast
