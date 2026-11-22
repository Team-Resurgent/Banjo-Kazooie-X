// lusx_menu.cpp -- the port's own menu, drawn over the game. Back, pressed and let go on its own, opens and closes it.
// After Diddy-Kong-Racing-X's dkr_menu.c.
//
// While it is open the game does not draw on: Lighthouse's frame (Interpolation/FramePacing.cpp, XboxRunMenu) sits
// on the frame it was drawing, replaying its recording with the menu over it, and goes back to the game once the menu
// has closed; the game's tick waits on that frame being done, as it waits on the N64's RDP.
//
// The controls are Lighthouse's own mappings (port 1's, what its input editor changes on a PC): a row shows the Xbox
// inputs on an N64 button, and they're kept in the config file with the rest. The Cheats and Options pages are
// Lighthouse's settings (kCheatGroups, kOptionGroups, the rows of its PC menu: src/port/UI/LighthouseMenu*.cpp), saved
// once as the menu closes.
#include <memory>
#include <stdio.h>
#include <string>
#include <vector>

#include <SDL2/SDL.h>
#include <spdlog/spdlog.h>

#include "ship/Context.h"
#include "ship/config/ConsoleVariable.h"
#include "ship/controller/controldeck/ControlDeck.h"
#include "ship/controller/controldevice/controller/Controller.h"
#include "ship/controller/controldevice/controller/ControllerButton.h"
#include "ship/controller/controldevice/controller/mapping/sdl/SDLAxisDirectionToButtonMapping.h"
#include "ship/controller/controldevice/controller/mapping/sdl/SDLButtonToButtonMapping.h"
#include "libultraship/libultra/controller.h"
#include "libultraship/bridge/consolevariablebridge.h"
#include "port/ShipInit.hpp"

// after the C++ headers: the Xbox's xtl.h defines min and max (and ignores NOMINMAX)
#include <xtl.h>

#include "lusx_debug.h"
#include "lusx_log.h"
#include "lusx_menu.h"
#include "lusx_options.h"
#include "lusx_overlay.h"

// the layout, in lusx_overlay.h's 960x720
#define PANEL_X0 150.0f
#define PANEL_X1 810.0f
#define PANEL_Y0 60.0f
#define PANEL_Y1 676.0f
#define BORDER 4.0f
#define RADIUS 30.0f
#define HEADER_Y1 154.0f
#define TITLE_Y 107.0f
#define ROWS_Y 208.0f
#define ROW_STEP 58.0f
#define ROW_HALF 24.0f
#define MAIN_STEP 52.0f // the main page's, a little closer: its eight rows clear of the footer
#define LIST_Y 192.0f
#define LIST_STEP 46.0f
#define LIST_HALF 20.0f
#define LIST_ROWS 9 // of the controls page, on screen at once
#define MOD_ROWS 7  // of the Mods page, with the chosen mod's two lines under them
#define MOD_INFO_Y 548.0f
#define MOD_INFO_STEP 34.0f
#define ROW_X0 186.0f
#define ROW_X1 774.0f
#define LABEL_X 214.0f
#define VALUE_X 744.0f
#define FOOTER_Y 634.0f
#define BADGE_RADIUS 15.0f
#define ARROW_GAP 12.0f
#define ARROW_SIZE 9.0f
#define CENTRE_X ((PANEL_X0 + PANEL_X1) * 0.5f)

// colours, 0xAARRGGBB
#define C_DIM 0xA0000010u
#define C_SHADOW 0x70000000u
#define C_GOLD_TOP 0xFFFFDA55u
#define C_GOLD_BOTTOM 0xFFE09A18u
#define C_PANEL_TOP 0xF5382A6Eu
#define C_PANEL_BOTTOM 0xF5140E2Cu
#define C_HEADER_TOP 0xFF6A4CC0u
#define C_HEADER_BOTTOM 0xFF3A2A80u
#define C_HEADER_MIDDLE 0xFF4A3896u
#define C_ROW 0x16FFFFFFu
#define C_CHOSEN_TOP 0xF07A5CE0u
#define C_CHOSEN_BOTTOM 0xF04A32A8u
#define C_TEXT_SHADOW 0x90000000u
#define C_TITLE 0xFFFFFFFFu
#define C_LABEL 0xFFE8E4FFu
#define C_VALUE 0xFFC8B8FFu
#define C_CHOSEN 0xFFFFFFFFu
#define C_CHOSEN_VALUE 0xFFFFE060u
#define C_HINT 0xFFC4BCEEu
#define C_BADGE_A 0xFF3DB24Au
#define C_BADGE_B 0xFFD8443Cu
#define C_BADGE_X 0xFF3C78D8u
#define C_BADGE_Y 0xFFD8A82Cu

#define REPEAT_AFTER 24 // frames a direction is held before it repeats
#define REPEAT_EVERY 6
#define SCROLL_HOLD 60    // frames a long label waits at each end
#define SCROLL_SPEED 1.5f // and the layout's pixels it moves a frame

static const unsigned kMenuLeft = LUSX_PAD_LEFT | LUSX_PAD_L_LEFT, kMenuRight = LUSX_PAD_RIGHT | LUSX_PAD_L_RIGHT;

enum Page { PAGE_MAIN, PAGE_CONTROLS, PAGE_CAPTURE, PAGE_GROUPS, PAGE_LIST, PAGE_DEBUG, PAGE_MODS, PAGE_CONFIRM };

enum MainRow { ROW_RESUME, ROW_CONTROLS, ROW_CHEATS, ROW_OPTIONS, ROW_MODS, ROW_DEBUG, ROW_RESTART, ROW_EXIT, ROW_COUNT };

// the Debug page: the switches always shown first, then the tools Debug mode shows (Warp and Edit save), then Back
enum DebugRow {
    DEBUG_MODE,
    DEBUG_LOGGING, // lusx_log.h
#if LUSX_DEBUG_TOOLS
    DEBUG_OVERLAY,
#endif
    DEBUG_WARP, // the Warp page: an area and its entrance (port/lhx_save_edit.cpp)
    DEBUG_EDIT,
    DEBUG_BACK,
    DEBUG_COUNT
};

// the N64's buttons, as the controls page lists them
struct N64Button {
    const char* name;
    CONTROLLERBUTTONS_T mask;
};
static const N64Button kButtons[] = {
    { "A", BTN_A },           { "B", BTN_B },           { "Z", BTN_Z },
    { "R", BTN_R },           { "L", BTN_L },           { "Start", BTN_START },
    { "C-Up", BTN_CUP },      { "C-Down", BTN_CDOWN },  { "C-Left", BTN_CLEFT },
    { "C-Right", BTN_CRIGHT }, { "D-Pad Up", BTN_DUP },  { "D-Pad Down", BTN_DDOWN },
    { "D-Pad Left", BTN_DLEFT }, { "D-Pad Right", BTN_DRIGHT },
};
enum {
    BUTTON_COUNT = sizeof(kButtons) / sizeof(kButtons[0]),
    LIST_RUMBLE = BUTTON_COUNT,
    LIST_RESET,
    LIST_BACK,
    LIST_COUNT
};

// the Xbox's inputs that can go on one. Not Back, which is the menu's, nor the left stick, which is the N64's stick.
struct XboxInput {
    unsigned bit;
    const char* name;
    int button;         // an SDL button, or
    int axis, direction; // an SDL axis' half
};
static const XboxInput kInputs[] = {
    { LUSX_PAD_A, "A", SDL_CONTROLLER_BUTTON_A, 0, 0 },
    { LUSX_PAD_B, "B", SDL_CONTROLLER_BUTTON_B, 0, 0 },
    { LUSX_PAD_X, "X", SDL_CONTROLLER_BUTTON_X, 0, 0 },
    { LUSX_PAD_Y, "Y", SDL_CONTROLLER_BUTTON_Y, 0, 0 },
    { LUSX_PAD_BLACK, "Black", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0, 0 },
    { LUSX_PAD_WHITE, "White", SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 0, 0 },
    { LUSX_PAD_LT, "Left trigger", -1, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 1 },
    { LUSX_PAD_RT, "Right trigger", -1, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 1 },
    { LUSX_PAD_START, "Start", SDL_CONTROLLER_BUTTON_START, 0, 0 },
    { LUSX_PAD_LS, "Left click", SDL_CONTROLLER_BUTTON_LEFTSTICK, 0, 0 },
    { LUSX_PAD_RS, "Right click", SDL_CONTROLLER_BUTTON_RIGHTSTICK, 0, 0 },
    { LUSX_PAD_UP, "D-Up", SDL_CONTROLLER_BUTTON_DPAD_UP, 0, 0 },
    { LUSX_PAD_DOWN, "D-Down", SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0, 0 },
    { LUSX_PAD_LEFT, "D-Left", SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0, 0 },
    { LUSX_PAD_RIGHT, "D-Right", SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0, 0 },
    { LUSX_PAD_R_UP, "Stick Up", -1, SDL_CONTROLLER_AXIS_RIGHTY, -1 },
    { LUSX_PAD_R_DOWN, "Stick Down", -1, SDL_CONTROLLER_AXIS_RIGHTY, 1 },
    { LUSX_PAD_R_LEFT, "Stick Left", -1, SDL_CONTROLLER_AXIS_RIGHTX, -1 },
    { LUSX_PAD_R_RIGHT, "Stick Right", -1, SDL_CONTROLLER_AXIS_RIGHTX, 1 },
};
enum { INPUT_COUNT = sizeof(kInputs) / sizeof(kInputs[0]) };

// The Cheats and Options pages: rows over Lighthouse's own settings, the ones its PC menu changes
// (src/port/UI/LighthouseMenuEnhancements.cpp and LighthouseMenuSettings.cpp, by the same names, with the defaults the
// game reads them with). A setting's hooks are made again when it changes (ShipInit::Init), as on a PC.
#define OPT_SWITCH(label, cvar) { OPT_ON_OFF, label, cvar, 0, NULL, NULL, 0, -1, NULL }
#define OPT_SWITCH_ON(label, cvar) { OPT_ON_OFF, label, cvar, 1, NULL, NULL, 0, -1, NULL }
#define OPT_NAMES(label, cvar, names) { OPT_LIST, label, cvar, 0, names, NULL, ARRAY_LEN(names), -1, NULL }
#define OPT_VALUES(label, cvar, fallback, names, values) \
    { OPT_LIST, label, cvar, fallback, names, values, ARRAY_LEN(names), -1, NULL }
// Lighthouse's volumes are percentages, read again by its hooks (Audio/AudioVolume.cpp, OS/OS_AI.cpp); `tenths`, its
// default for one never set
#define OPT_VOLUME(label, cvar, tenths) { OPT_PERCENT, label, cvar, tenths, NULL, NULL, 11, -1, NULL }
#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

static const char* const kBootNames[] = { "N64 logos", "Skip to file select" };
static const int kBootValues[] = { 1, 2 }; // BOOTSEQUENCE_AUTHENTIC, BOOTSEQUENCE_FILESELECT (UI/enhancementTypes.h)
static const char* const kFurnaceFunNames[] = { "Off", "At start", "Console timing" };
static const char* const kDamageNames[] = { "Normal", "Hard", "Brutal", "One hit" };
static const int kDamageValues[] = { 1, 2, 3, 4 };
static const char* const kFollowNames[] = { "Hold R", "Always", "Toggle with R" };
static const char* const kDrawDistanceNames[] = { "Normal", "2x", "3x", "4x", "5x", "6x" };
static const int kDrawDistanceValues[] = { 1, 2, 3, 4, 5, 6 };
static const char* const kRumbleNames[] = { "Full", "Medium", "Low", "Off" };

// The Cheats page: Lighthouse's Cheats (Enhancements, Cheats), less the cosmetics editor and the timer, windows
static const Option kCheatInfinite[] = {
    OPT_SWITCH("Health", "gEnhancements.Cheats.InfiniteHealth"),
    OPT_SWITCH("Air", "gEnhancements.Cheats.InfiniteAir"),
    OPT_SWITCH("Lives", "gEnhancements.Cheats.InfiniteLives"),
    OPT_SWITCH("Eggs", "gEnhancements.Cheats.InfiniteEggs"),
    OPT_SWITCH("Red feathers", "gEnhancements.Cheats.InfiniteRedFeathers"),
    OPT_SWITCH("Gold feathers", "gEnhancements.Cheats.InfiniteGoldFeathers"),
    OPT_SWITCH("Wading Boots and Turbo Trainers", "gEnhancements.Cheats.InfiniteBootsSneakers"),
};

static const Option kCheatMovement[] = {
    OPT_SWITCH("Hold L to levitate", "gEnhancements.Cheats.Levitate"),
    OPT_SWITCH("Press L to fly anywhere", "gEnhancements.Cheats.FlyAnywhere"),
    OPT_SWITCH("D-pad Talon Trot cycling", "gEnhancements.Cheats.TalonTrotCycle"),
};

static const Option kCheatTransform[] = {
    OPT_SWITCH("Fast transformations", "gEnhancements.Cheats.FastTransform"),
    OPT_SWITCH("D-pad cycles transformations", "gEnhancements.Cheats.CycleTransform"),
    OPT_SWITCH("No Mumbo untransform", "gEnhancements.Cheats.NoMumboUntransform"),
};

// on the Debug page: shows Warp and Edit save. The port's own setting: Lighthouse's Debug Mode has no row in its PC
// menu (UI/LighthouseMenuDevTools.cpp) and nothing in the game reads it. Logged as the cheats are.
static const Option kDebugMode = OPT_SWITCH("Debug mode", "gLusx.DebugMode");

// on the Debug page, whatever Debug mode is: lusx_log.h. On on a fresh install while the port is new (the other ports': off), so a test run's
// log and crash report are there from its first start.
static const Option kLogging = { OPT_ON_OFF, "Logging", "gLusx.Logging", 1, NULL, NULL, 0, -1, LusxLog_Apply };

// the spdlog level that Logging's Off replaces, given back by On
static spdlog::level::level_enum sLogLevel = spdlog::level::info;

extern "C" void LusxLog_Apply(void) {
    static bool sSettled;
    const bool on = CVarGetInteger(kLogging.cvar, kLogging.fallback) != 0;
    const std::shared_ptr<spdlog::logger> logger = spdlog::default_logger();

    if (!sSettled) { // the first, as the config is read: the start-up's held lines go out, or are dropped
        sSettled = true;
        if (!on) {
            sLogLevel = logger->level();
            logger->set_level(spdlog::level::off);
        }
        LusxLog_Settle(on);
        return;
    }
    if (!on && gLusxLogOn) {
        SPDLOG_INFO("lus-x: logging off (port menu, Debug page)");
        sLogLevel = logger->level();
        logger->set_level(spdlog::level::off);
        LusxLog_Settle(0);
    } else if (on && !gLusxLogOn) {
        LusxLog_Settle(1);
        logger->set_level(sLogLevel);
        SPDLOG_INFO("lus-x: logging on (port menu, Debug page)");
    }
}

#if LUSX_DEBUG_TOOLS
// on the Debug page: the tester build's overlay, and the last run's report on it (lusx_debug.h).
// Saved, off on a fresh install.
static void ApplyOverlay(void) {
    LusxDebug_SetOverlay(CVarGetInteger("gLusx.TestOverlay", 0));
}
static const Option kTestOverlay = { OPT_ON_OFF, "Test overlay", "gLusx.TestOverlay", 0, NULL, NULL, 0, -1, ApplyOverlay };
#endif

// The Options page: Lighthouse's Enhancements and Settings menus, the rows that make sense with no PC windows. Left
// out: the aiming crosshairs (Beak Bomb aiming, first-person egg aim: drawn by ImGui), Always update actors (every
// actor in a level updated each tick: the Xbox's CPU is the N64 game's budget), mipmaps (always on, as Lighthouse's
// default: off, the ground's textures change with distance) and the control schemes (they rewrite the controls page's
// mappings).
static const Option kCutscenes[] = {
    OPT_VALUES("At start-up", "gSettings.BootSequence", 1, kBootNames, kBootValues),
    OPT_SWITCH("Start skips the boot logos", "gEnhancements.Cutscenes.SkipBootLogos"),
    OPT_SWITCH("Start skips the intro", "gEnhancements.Cutscenes.StartSkipIntro"),
    OPT_SWITCH("Start skips other cutscenes", "gEnhancements.Cutscenes.SkipMiscCutscenes"),
    OPT_SWITCH("Skip the Jiggy dance", "gEnhancements.Cutscenes.SkipJiggyDance"),
    OPT_SWITCH("Skip the Clucker cutscene", "gEnhancements.Cutscenes.SkipCluckerCutscene"),
    OPT_SWITCH("Skip the note door dance", "gEnhancements.Cutscenes.SkipNoteDoorDance"),
    OPT_SWITCH("Skip the Spiral Mountain tutorial", "gEnhancements.Gameplay.SkipSMTutorial"),
};

static const Option kGameplay[] = {
    OPT_VALUES("Damage taken", "gEnhancements.Gameplay.DamageScale", 1, kDamageNames, kDamageValues),
    OPT_SWITCH("Permadeath", "gEnhancements.Gameplay.Permadeath"),
    OPT_SWITCH("No Snacker", "gEnhancements.Gameplay.DisableSnackerSpawn"),
    OPT_SWITCH("Extra time in the water pyramid", "gEnhancements.Gameplay.WaterPyramidTimer"),
    OPT_SWITCH("Easier Boggy races", "gEnhancements.Gameplay.EasierBoggyRaces"),
    OPT_SWITCH("Easier Mr. Vile", "gEnhancements.Gameplay.EasierMrVile"),
    OPT_NAMES("Furnace Fun moves", "gEnhancements.Gameplay.FurnaceFunMoves", kFurnaceFunNames),
    OPT_SWITCH("Stop 'n' Swop at 100%", "gEnhancements.Gameplay.StopNSwop100"),
    OPT_SWITCH("Restore Return to Lair", "gEnhancements.Gameplay.ReturnToLair"),
    OPT_SWITCH("Highlight Brentilda's hints", "gEnhancements.Gameplay.HighlightHeardHints"),
    OPT_SWITCH("No voice pitch shift", "gEnhancements.Gameplay.DisableVoicePitchShift"),
    OPT_SWITCH("Tooie Jiggy animation", "gEnhancements.Backports.JiggyAnimation"),
    OPT_SWITCH("Honeyback health regen", "gEnhancements.Backports.Honeyback"),
    OPT_SWITCH("Fast swimming", "gEnhancements.Backports.FastSwim"),
    OPT_SWITCH("Tooie roll", "gEnhancements.Backports.Roll"),
    OPT_SWITCH("Tooie Beak Bomb", "gEnhancements.Backports.BeakBomb"),
    OPT_SWITCH("Mirrored world", "gEnhancements.Modes.MirroredWorld.Mode"),
};

static const Option kSaving[] = {
    OPT_SWITCH("Keep Bottles' bonuses", "gEnhancements.Saving.PersistBottlesBonus"),
    OPT_SWITCH("Keep extra lives", "gEnhancements.Saving.PersistExtraLives"),
    OPT_SWITCH("Keep collected notes", "gEnhancements.Gameplay.NoteRetention"),
    OPT_SWITCH("Keep collected Jinjos", "gEnhancements.Gameplay.JinjoRetention"),
    OPT_SWITCH("Keep collected consumables", "gEnhancements.Gameplay.ConsumableRetention"),
};

static const Option kCamera[] = {
    OPT_NAMES("Follow camera", "gEnhancements.Camera.Follow", kFollowNames),
    OPT_SWITCH("Invert camera left and right", "gEnhancements.Camera.InvertX"),
    OPT_SWITCH("Invert first-person left and right", "gEnhancements.Camera.FirstPerson.InvertX"),
    OPT_SWITCH("Invert first-person up and down", "gEnhancements.Camera.InvertY"),
    OPT_SWITCH("Invert swimming and flying up and down", "gLusx.InvertSwimFlyY"), // (the port's own)
    OPT_SWITCH("Free look on the right stick", "gEnhancements.Camera.FreeLook.Enabled"),
    OPT_SWITCH("Free look: invert left and right", "gEnhancements.Camera.FreeLook.InvertX"),
    OPT_SWITCH("Free look: invert up and down", "gEnhancements.Camera.FreeLook.InvertY"),
    OPT_SWITCH("Toggle Talon Trot", "gSettings.Controls.TalonTrotToggle"),
    OPT_SWITCH("No screen shake", "gSettings.A11yDisableScreenShake"),
};

static const Option kFixes[] = {
    OPT_SWITCH("Fix the Furnace Fun game over text", "gEnhancements.Fixes.FurnaceFunDialog"),
    OPT_SWITCH("Fix void-out game overs", "gEnhancements.Fixes.VoidOutGameOver"),
    OPT_SWITCH_ON("Fix Boggy race game overs", "gEnhancements.Fixes.BoggyRaceGameOver"),
    OPT_SWITCH("Fix Mumbo Token: GV water pyramid", "gEnhancements.Fixes.MumboTokenGV"),
    OPT_SWITCH("Fix Mumbo Token: MMM Loggo", "gEnhancements.Fixes.MumboTokenMMM"),
    OPT_SWITCH("Fix Mumbo Token: CCW Spring", "gEnhancements.Fixes.MumboTokenCCW"),
    OPT_SWITCH_ON("Fix the CCW flower replant softlock", "gEnhancements.Fixes.CCWFlowerReplant"),
    OPT_SWITCH("Fix the CCW Gnawty rock (Spring)", "gEnhancements.Fixes.GnawtySpringRock"),
    OPT_SWITCH("Fix termite mound slopes", "gEnhancements.Fixes.TermiteMoundSlopes"),
    OPT_SWITCH("Fix early claw swipe in a slide", "gEnhancements.Fixes.ClawSwipeSlide"),
    OPT_SWITCH("Fix the Grunty defeated flag", "gEnhancements.Fixes.GruntyDefeatedFlag"),
    OPT_SWITCH_ON("Fix bouncing Grunty", "gEnhancements.Fixes.GruntyBounce"),
    OPT_SWITCH("Fix Grunty's Jinjo charge sound", "gEnhancements.Fixes.JinjoChargeSound"),
    OPT_SWITCH_ON("Mute the Chimpy replay", "gEnhancements.Fixes.ChimpyStumpRumble"),
    OPT_SWITCH_ON("Fix cutscene audio sync", "gEnhancements.Fix.CutsceneSync"),
    OPT_SWITCH_ON("Fix widescreen oddities", "gEnhancements.Fix.WidescreenCamera"),
    OPT_SWITCH("Fix Conga's name", "gEnhancements.Fixes.CongaText"),
    OPT_SWITCH("Fix the Freezeezy Peak lobby", "gEnhancements.Fixes.FPLobbyDoorTile"),
};

static const Option kDisplay[] = {
    // the frames between the game's ticks, blended on the GPU (Interpolation/FramePacing.cpp); off, the game's own rate
    OPT_SWITCH_ON("60 fps", "gLusx.Fps60"),
    OPT_VALUES("Draw distance", "gEnhancements.Graphics.DrawDistance", 1, kDrawDistanceNames, kDrawDistanceValues),
    // a texture pack's art in place of the game's (mods/ beside default.xbe: tools/hd_pack.py), as the PC's Tab
    // key toggles it: Lighthouse's alternate assets, applied as the frame ends (FramePacing.cpp SyncAltAssets)
    OPT_SWITCH("HD textures", "gSettings.Mods.AlternateAssets"), // (off by default, as Engine.cpp reads it)
    OPT_SWITCH("Disable level of detail", "gEnhancements.Graphics.DisableLOD"),
    OPT_SWITCH("Original aspect ratio in cutscenes", "gEnhancements.Graphics.CutsceneAspect"),
    OPT_VOLUME("Master volume", "gSettings.Volume.Master", 4),
    OPT_VOLUME("Music", "gSettings.Volume.Music", 10),
    OPT_VOLUME("Sound effects", "gSettings.Volume.SFX", 10),
};

// on the controls page
static const Option kRumble = OPT_NAMES("Rumble", "gLusx.Rumble", kRumbleNames);

// (six at most: the page doesn't scroll)
static const OptionGroup kOptionGroups[] = {
    { "Cutscenes", kCutscenes, ARRAY_LEN(kCutscenes) },
    { "Gameplay", kGameplay, ARRAY_LEN(kGameplay) },
    { "Saving", kSaving, ARRAY_LEN(kSaving) },
    { "Camera and controls", kCamera, ARRAY_LEN(kCamera) },
    { "Fixes", kFixes, ARRAY_LEN(kFixes) },
    { "Display and sound", kDisplay, ARRAY_LEN(kDisplay) },
};
static const OptionGroup kCheatGroups[] = {
    { "Infinite", kCheatInfinite, ARRAY_LEN(kCheatInfinite) },
    { "Movement", kCheatMovement, ARRAY_LEN(kCheatMovement) },
    { "Transformations", kCheatTransform, ARRAY_LEN(kCheatTransform) },
};

static volatile int sOpen;       // drawn and holding the game
static volatile unsigned sPad;   // the pads, as last polled
static volatile int sWanted;     // Back was pressed and let go
static int sBackDown, sBackAlone;
static int sClosing;             // closed, but the buttons that closed it are not yet let go
static Page sPage;
static int sCursor, sTop;        // the chosen row; the controls page's first on screen
static int sMainCursor;          // what the main page goes back to
static bool sAdd;                // the input being asked for goes on beside the ones there
static int sConfirmRow;          // the main page's row being confirmed
static bool sConfirmFromMods;    // the restart was asked for on the Mods page, which No goes back to
static int sHeldFor;             // frames a direction has been held
static unsigned sHeld;           // last frame's buttons
static unsigned sPressed;        // every button pressed since the menu opened
static std::string sValues[BUTTON_COUNT]; // what each N64 button is on, for the controls page
static const Option* sList;      // the list page's rows, its title and how many, with a Back row after them
static const char* sListTitle;
static int sListCount;
static const OptionGroup* sGroups; // the Options or Cheats page: its rows, how many, with a Back row after, its title
static int sGroupCount;
static const char* sGroupsTitle;
static int sGroupCursor;         // what the list page goes back to there
static bool sListFromDebug;      // the list page was opened from the Debug page (Warp)
static int sDebugCursor;         // what the pages opened from the Debug page go back to
static int sPinned;              // the list page's first rows that stay put while the rest scroll (none on Banjo's)
static int sListRow;             // the row of the rest the cursor was last on
static int sScrollFrames;        // frames the cursor has been on this row, for a long label
static bool sChanged;            // settings to save when the menu closes

static std::shared_ptr<Ship::Controller> Pad1(void) {
    return Ship::Context::GetRawInstance()->GetControlDeck()->GetControllerByPort(0);
}

static std::shared_ptr<Ship::ControllerButtonMapping> MappingFor(const XboxInput& in, CONTROLLERBUTTONS_T mask) {
    if (in.button >= 0) {
        return std::make_shared<Ship::SDLButtonToButtonMapping>(0, mask, in.button);
    }
    return std::make_shared<Ship::SDLAxisDirectionToButtonMapping>(0, mask, in.axis, in.direction);
}

// the rows' text, in kInputs' order
static void ReadMappings(void) {
    auto pad = Pad1();

    for (int b = 0; b < BUTTON_COUNT; b++) {
        auto button = pad->GetButton(kButtons[b].mask);
        std::string& value = sValues[b];

        value.clear();
        for (int i = 0; i < INPUT_COUNT; i++) {
            if (button->GetButtonMappingById(MappingFor(kInputs[i], kButtons[b].mask)->GetButtonMappingId()) != nullptr) {
                value += value.empty() ? "" : ", ";
                value += kInputs[i].name;
            }
        }
        if (value.empty()) {
            value = "None";
        }
    }
}

static void HaveConfig(void) {
    Ship::Context::GetRawInstance()->GetConsoleVariables()->SetInteger(CVAR_PREFIX_CONTROLLERS ".Port1.HasConfig", 1);
}

static void Remap(int row, const XboxInput& in) {
    auto cvars = Ship::Context::GetRawInstance()->GetConsoleVariables();
    auto button = Pad1()->GetButton(kButtons[row].mask);
    auto mapping = MappingFor(in, kButtons[row].mask);

    cvars->HoldSaves(true); // each step of this saves the whole file
    if (!sAdd) {
        button->ClearAllButtonMappingsForDeviceType(Ship::PhysicalDeviceType::SDLGamepad);
    }
    button->AddButtonMapping(mapping);
    mapping->SaveToConfig();
    button->SaveButtonMappingIdsToConfig();
    HaveConfig();
    cvars->HoldSaves(false);
    ReadMappings();
}

static void ResetMappings(void) {
    auto cvars = Ship::Context::GetRawInstance()->GetConsoleVariables();
    auto pad = Pad1();

    cvars->HoldSaves(true);
    for (int b = 0; b < BUTTON_COUNT; b++) {
        auto button = pad->GetButton(kButtons[b].mask);
        button->ClearAllButtonMappingsForDeviceType(Ship::PhysicalDeviceType::SDLGamepad);
        button->AddDefaultMappings(Ship::PhysicalDeviceType::SDLGamepad);
    }
    HaveConfig();
    cvars->HoldSaves(false);
    ReadMappings();
}

// a row's setting as an index into its values
static int OptionIndex(const Option& o) {
    if (o.kind == OPT_PERCENT) {
        const int tenths = (CVarGetInteger(o.cvar, o.fallback * 10) + 5) / 10;
        return tenths < 0 ? 0 : tenths > 10 ? 10 : tenths;
    }
    const int value = CVarGetInteger(o.cvar, o.fallback);
    if (o.values != NULL) {
        for (int i = 0; i < o.count; i++) {
            if (o.values[i] == value) {
                return i;
            }
        }
        return 0;
    }
    return value;
}

// its value's text; `text` is for the ones that have to be made
static const char* OptionText(const Option& o, char* text, size_t size) {
    if (o.kind == OPT_CUSTOM || o.kind == OPT_ACTION) {
        return o.text(o.arg, text, size);
    }
    const int index = OptionIndex(o);

    if (o.kind == OPT_ON_OFF) {
        return index ? "On" : "Off";
    }
    if (o.kind == OPT_PERCENT) {
        snprintf(text, size, "%d%%", index * 10);
        return text;
    }
    return index >= 0 && index < o.count ? o.names[index] : "?";
}

static void ChangeOption(const Option& o, int step) {
    if (o.kind == OPT_CUSTOM || o.kind == OPT_ACTION) { // an edit of the save: a report from a run with them says so
        char mark[80], text[32];
        o.step(o.arg, step);
        snprintf(mark, sizeof(mark), "save edit: %s: %s", o.label, OptionText(o, text, sizeof(text)));
        LusxDebug_Mark(mark);
        return;
    }
    const int count = o.kind == OPT_ON_OFF ? 2 : o.count;
    const int index = (OptionIndex(o) + step + count) % count;

    if (o.kind == OPT_PERCENT) {
        CVarSetInteger(o.cvar, index * 10);
    } else {
        CVarSetInteger(o.cvar, o.values != NULL ? o.values[index] : index);
    }
    if (o.after != NULL) {
        o.after();
    }
    ShipInit::Init(o.cvar);
    sChanged = true;
    if (&o == &kDebugMode || (sList != NULL && sGroups == kCheatGroups)) { // a report from a run with cheats on says so
        char mark[80], text[32];
        snprintf(mark, sizeof(mark), "cheat: %s: %s", o.label, OptionText(o, text, sizeof(text)));
        LusxDebug_Mark(mark);
    }
}

static void SaveSettings(void) {
    if (sChanged) {
        sChanged = false;
        CVarSave();
    }
}

// the log says what's on at the start of a run too: the settings outlive it
void LusxMenu_LogCheats(void) {
    char text[32];

#if LUSX_DEBUG_TOOLS
    ApplyOverlay(); // the saved Test overlay (and with it, whether the last run's report is shown)
#endif
    if (OptionIndex(kDebugMode) != 0) {
        SPDLOG_INFO("lus-x: cheat on: {}", kDebugMode.label);
    }
    for (const OptionGroup& group : kCheatGroups) {
        for (int i = 0; i < group.count; i++) {
            const Option& o = group.list[i];
            if (OptionIndex(o) != 0) {
                SPDLOG_INFO("lus-x: cheat on: {}: {}", o.label, OptionText(o, text, sizeof(text)));
            }
        }
    }
}

// what the game is left for: an empty image is the dashboard
static void Leave(const char* image, const char* why) {
    SaveSettings();
    LusxDebug_Mark(why);
    LusxDebug_Stop();
    XLaunchNewImage(image, NULL);
    XLaunchNewImage(NULL, NULL);
}

extern "C" void LusxMenu_Pad(unsigned buttons) {
    sPad = buttons;
    if (sOpen) {
        return;
    }
    if (buttons & LUSX_PAD_BACK) {
        if (!sBackDown) {
            sBackDown = 1;
            sBackAlone = 1;
        }
        // the tester tools' lock test starts with Back too
        if (buttons & (LUSX_PAD_LS | LUSX_PAD_RS)) {
            sBackAlone = 0;
        }
    } else if (sBackDown) {
        sBackDown = 0;
        sWanted = sBackAlone;
    }
}

extern "C" int LusxMenu_Wanted(void) {
    const int wanted = sWanted;
    sWanted = 0;
    return wanted;
}

extern "C" int LusxMenu_IsOpen(void) {
    return sOpen;
}

bool LusxMenu_Open(Fast::GfxRenderingAPI* rapi) {
    if (!Ovl_Load(rapi)) {
        return false;
    }
    sOpen = 1;
    sClosing = 0;
    sPage = PAGE_MAIN;
    sCursor = sMainCursor = 0;
    sTop = 0;
    sHeld = sPad; // what's held already isn't a press
    sPressed = 0;
    sHeldFor = 0;
    LusXbox_PadsQuiet();
    ReadMappings();
    LusxDebug_Mark("menu: opened");
    return true;
}

extern "C" void LusxMenu_Close(void) {
    SaveSettings();
    LusxWarp_Run();
    for (std::string& value : sValues) {
        std::string().swap(value);
    }
    Ovl_Unload();
    sOpen = 0;
    LusxDebug_Mark("menu: closed");
}

static void GoMain(void) {
    sPage = PAGE_MAIN;
    sCursor = sMainCursor;
}

static void GoList(const Option* list, int count, const char* title) {
    sPage = PAGE_LIST;
    sList = list;
    sListCount = count;
    sListTitle = title;
    sListFromDebug = false;
    sPinned = 0;
    sCursor = sTop = sListRow = 0;
}

// back to the page a list was opened from
static void GoGroups(void) {
    sPage = PAGE_GROUPS;
    sCursor = sGroupCursor;
}

static void GoDebug(void) {
    sPage = PAGE_DEBUG;
    sCursor = sDebugCursor;
    sList = NULL; // (Debug mode's row isn't in a list)
}

static void LeaveList(void) {
    if (sListFromDebug) {
        GoDebug();
    } else {
        GoGroups();
    }
}

// the Options and Cheats pages go back to the main page, Edit save's to the Debug page
static void LeaveGroups(void) {
    if (sGroups == kEditGroups) {
        GoDebug();
    } else {
        GoMain();
    }
}

static void OpenGroups(const OptionGroup* groups, int count, const char* title) {
    sGroups = groups;
    sGroupCount = count;
    sGroupsTitle = title;
    sGroupCursor = 0;
    GoGroups();
}

// Warp and Edit save are debug tools: Debug mode shows them
static bool ShowDebugRows(void) {
    return CVarGetInteger(kDebugMode.cvar, 0) != 0;
}

static bool IsDebugTool(int row) {
    return row == DEBUG_WARP || row == DEBUG_EDIT;
}

static void UpdateDebug(unsigned pressed, int step) {
    if (step != 0) {
        do {
            sCursor = (sCursor + step + DEBUG_COUNT) % DEBUG_COUNT;
        } while (IsDebugTool(sCursor) && !ShowDebugRows());
    }
    if (pressed & LUSX_PAD_B) {
        GoMain();
        return;
    }
    if (sCursor == DEBUG_MODE) {
        if (pressed & (kMenuLeft | kMenuRight)) {
            ChangeOption(kDebugMode, 1);
        }
        return;
    }
    if (sCursor == DEBUG_LOGGING) {
        if (pressed & (kMenuLeft | kMenuRight)) {
            ChangeOption(kLogging, 1);
        }
        return;
    }
#if LUSX_DEBUG_TOOLS
    if (sCursor == DEBUG_OVERLAY) {
        if (pressed & (kMenuLeft | kMenuRight)) {
            ChangeOption(kTestOverlay, 1);
        }
        return;
    }
#endif
    if (!(pressed & LUSX_PAD_A)) {
        return;
    }
    sDebugCursor = sCursor;
    if (sCursor == DEBUG_WARP) {
        // only with a save being played: a warp from the title or the file select would start the game on no file
        if (LusxEdit_Available()) {
            const Option* list;
            int count, cursor, pinned;

            LusxWarp_Open(&list, &count, &cursor, &pinned);
            GoList(list, count, "Warp");
            // ready on the last warp's area (Majora's-X's starts on its settings, pinned above; Banjo's warps have none)
            sPinned = pinned;
            sCursor = sListRow = cursor;
            sTop = cursor - 2 > pinned ? cursor - 2 : pinned;
            sListFromDebug = true;
        }
    } else if (sCursor == DEBUG_EDIT) {
        if (LusxEdit_Available()) {
            OpenGroups(kEditGroups, kEditGroupCount, "Edit save");
        }
    } else {
        GoMain();
    }
}

// The Mods page: the archives in mods/ beside default.xbe, as this run's start found them (port/lhx_nomods.cpp), each
// on or off, in their order (X and Y move one up and down: the top one wins where two change the same thing). They're
// only read at the start, so a change waits for a restart, and a "Restart to apply" row is there while one does. (As
// Mario-Kart-64XX's, with the order.)
static bool ModMoved(int i) {
    return LusxMods_List()[i].start != i;
}

static bool ModsChanged(void) {
    const std::vector<LusxMod>& mods = LusxMods_List();
    for (int i = 0; i < (int)mods.size(); i++) {
        if (LusxMods_IsOff(mods[i].file) == mods[i].on || ModMoved(i)) {
            return true;
        }
    }
    return false;
}

// its rows: the mods, Restart to apply while there's a change, then Back
static int ModCount(void) {
    return (int)LusxMods_List().size();
}
static int ModRestartRow(void) {
    return ModsChanged() ? ModCount() : -1;
}
static int ModRows(void) {
    return ModCount() + (ModsChanged() ? 1 : 0) + 1;
}

static void GoMods(int cursor) {
    sPage = PAGE_MODS;
    sCursor = cursor;
    sTop = sCursor >= MOD_ROWS ? sCursor - MOD_ROWS + 1 : 0;
}

static void UpdateMods(unsigned pressed, int step) {
    const int rows = ModRows();

    sCursor = (sCursor + step + rows) % rows;
    sTop = sCursor < sTop ? sCursor : sCursor >= sTop + MOD_ROWS ? sCursor - MOD_ROWS + 1 : sTop;
    if (pressed & LUSX_PAD_B) {
        GoMain();
    } else if (sCursor < ModCount()) {
        if (pressed & (LUSX_PAD_A | kMenuLeft | kMenuRight)) {
            const LusxMod& mod = LusxMods_List()[sCursor];
            const bool off = !LusxMods_IsOff(mod.file);
            char mark[96];

            LusxMods_SetOff(mod.file, off);
            sChanged = true;
            snprintf(mark, sizeof(mark), "mods: %s %s", mod.file.c_str(), off ? "off" : "on");
            LusxDebug_Mark(mark);
        } else if (pressed & (LUSX_PAD_X | LUSX_PAD_Y)) {
            // up or down, the cursor with it
            const int by = (pressed & LUSX_PAD_X) ? -1 : 1;
            if (sCursor + by >= 0 && sCursor + by < ModCount()) {
                char mark[96];

                snprintf(mark, sizeof(mark), "mods: %s %s", LusxMods_List()[sCursor].file.c_str(), by < 0 ? "up" : "down");
                LusxMods_Move(sCursor, by);
                sCursor += by;
                sChanged = true;
                LusxDebug_Mark(mark);
                sTop = sCursor < sTop ? sCursor : sCursor >= sTop + MOD_ROWS ? sCursor - MOD_ROWS + 1 : sTop;
            }
        }
        // the Restart row comes and goes after the mods (the cursor stays on a mod): no gap left at the end
        const int lastTop = ModRows() > MOD_ROWS ? ModRows() - MOD_ROWS : 0;
        sTop = sTop > lastTop ? lastTop : sTop;
    } else if (pressed & LUSX_PAD_A) {
        if (sCursor == ModRestartRow()) {
            sPage = PAGE_CONFIRM;
            sConfirmRow = ROW_RESTART;
            sConfirmFromMods = true;
            sCursor = 0; // No: the safe answer first
        } else {
            GoMain();
        }
    }
}

static void UpdateMain(unsigned pressed, int step) {
    sCursor = (sCursor + step + ROW_COUNT) % ROW_COUNT;
    if (pressed & LUSX_PAD_B) {
        sClosing = 1;
    }
    if (!(pressed & LUSX_PAD_A)) {
        return;
    }
    sMainCursor = sCursor;
    switch (sCursor) {
        case ROW_RESUME:
            sClosing = 1;
            break;
        case ROW_CONTROLS:
            sPage = PAGE_CONTROLS;
            sCursor = sTop = 0;
            sList = NULL; // (Rumble's row isn't a cheat, whatever list was last open)
            break;
        case ROW_CHEATS:
            OpenGroups(kCheatGroups, ARRAY_LEN(kCheatGroups), "Cheats");
            break;
        case ROW_OPTIONS:
            OpenGroups(kOptionGroups, ARRAY_LEN(kOptionGroups), "Options");
            break;
        case ROW_MODS:
            GoMods(0);
            break;
        case ROW_DEBUG:
            sDebugCursor = !ShowDebugRows() ? DEBUG_MODE : DEBUG_WARP;
            GoDebug();
            break;
        default:
            sPage = PAGE_CONFIRM;
            sConfirmRow = sCursor;
            sConfirmFromMods = false;
            sCursor = 0; // No: the safe answer first
            break;
    }
}

static void UpdateControls(unsigned pressed, int step) {
    sCursor = (sCursor + step + LIST_COUNT) % LIST_COUNT;
    sTop = sCursor < sTop ? sCursor : sCursor >= sTop + LIST_ROWS ? sCursor - LIST_ROWS + 1 : sTop;
    if (pressed & LUSX_PAD_B) {
        GoMain();
    } else if (sCursor == LIST_RUMBLE) {
        if (pressed & kMenuRight) {
            ChangeOption(kRumble, 1);
        } else if (pressed & kMenuLeft) {
            ChangeOption(kRumble, -1);
        }
    } else if (sCursor >= BUTTON_COUNT) {
        if (pressed & LUSX_PAD_A) {
            if (sCursor == LIST_RESET) {
                ResetMappings();
            } else {
                GoMain();
            }
        }
    } else if (pressed & (LUSX_PAD_A | LUSX_PAD_X)) {
        sAdd = (pressed & LUSX_PAD_A) == 0;
        sPage = PAGE_CAPTURE;
    }
}

static void UpdateGroups(unsigned pressed, int step) {
    sCursor = (sCursor + step + sGroupCount + 1) % (sGroupCount + 1);
    if (pressed & LUSX_PAD_B) {
        LeaveGroups();
    } else if (pressed & LUSX_PAD_A) {
        if (sCursor == sGroupCount) {
            LeaveGroups();
        } else {
            sGroupCursor = sCursor;
            GoList(sGroups[sCursor].list, sGroups[sCursor].count, sGroups[sCursor].title);
        }
    }
}

static void UpdateList(unsigned pressed, int step) {
    const int rows = sListCount + 1; // and Back
    const int shown = LIST_ROWS - sPinned; // the rows that scroll, at once

    if (sCursor < sPinned) {
        // the pinned rows and the rest are apart: up and down go round in each, and only X goes between them, back to
        // the row the list was left on
        if (pressed & LUSX_PAD_X) {
            sCursor = sListRow;
        } else {
            sCursor = (sCursor + step + sPinned) % sPinned;
        }
    } else {
        if (sPinned > 0 && (pressed & LUSX_PAD_X)) {
            sCursor = 0;
        } else {
            const int count = rows - sPinned;
            sCursor = sPinned + (sCursor - sPinned + step + count) % count;
        }
        // the triggers, a page at a time
        if (sCursor >= sPinned && (pressed & LUSX_PAD_LT)) {
            sCursor = sCursor - shown > sPinned ? sCursor - shown : sPinned;
        } else if (sCursor >= sPinned && (pressed & LUSX_PAD_RT)) {
            sCursor = sCursor + shown < rows ? sCursor + shown : rows - 1;
        }
    }
    if (sCursor >= sPinned) {
        sListRow = sCursor;
        sTop = sCursor < sTop ? sCursor : sCursor >= sTop + shown ? sCursor - shown + 1 : sTop;
    }
    if (pressed & LUSX_PAD_B) {
        LeaveList();
    } else if (sCursor == sListCount) {
        if (pressed & LUSX_PAD_A) {
            LeaveList();
        }
    } else if (sList[sCursor].kind == OPT_ACTION) {
        if (pressed & LUSX_PAD_A) { // done once, on purpose
            ChangeOption(sList[sCursor], 1);
        }
    } else if (sList[sCursor].press != NULL && (pressed & LUSX_PAD_A)) {
        char mark[96], text[48];

        snprintf(mark, sizeof(mark), "warp: %s: %s", sList[sCursor].label,
                 sList[sCursor].text(sList[sCursor].arg, text, sizeof(text)));
        LusxDebug_Mark(mark);
        sList[sCursor].press(sList[sCursor].arg);
        sChanged = true;
        sClosing = 1;
    } else if (pressed & kMenuRight) {
        ChangeOption(sList[sCursor], 1);
    } else if (pressed & kMenuLeft) {
        ChangeOption(sList[sCursor], -1);
    }
}

static void UpdateCapture(unsigned pressed) {
    for (int i = 0; i < INPUT_COUNT; i++) {
        if (pressed & kInputs[i].bit) {
            Remap(sCursor, kInputs[i]);
            sPage = PAGE_CONTROLS;
            return;
        }
    }
}

static void UpdateConfirm(unsigned pressed, int step) {
    sCursor = (sCursor + step + 2) % 2;
    if ((pressed & LUSX_PAD_B) || ((pressed & LUSX_PAD_A) && sCursor == 0)) {
        if (sConfirmFromMods) {
            GoMods(ModRestartRow() >= 0 ? ModRestartRow() : 0);
        } else {
            GoMain();
        }
    } else if (pressed & LUSX_PAD_A) {
        if (sConfirmRow == ROW_RESTART) {
            Leave("D:\\default.xbe", sConfirmFromMods ? "menu: restart (mods)" : "menu: restart");
        } else {
            Leave(NULL, "menu: to the dashboard");
        }
    }
}

extern "C" int LusxMenu_Update(void) {
    static const unsigned kUp = LUSX_PAD_UP | LUSX_PAD_L_UP, kDown = LUSX_PAD_DOWN | LUSX_PAD_L_DOWN;

    SDL_PumpEvents();
    const unsigned now = sPad, pressed = now & ~sHeld;
    sHeld = now;
    sPressed |= pressed;
    if (sClosing) {
        // back to the game only once the buttons pressed here are up: it would take one still held for a press of its
        // own. A button held from before the menu opened doesn't hold it up.
        return (now & sPressed) != 0;
    }
    // Back is the way out from anywhere, and all a capture can't take
    if (pressed & LUSX_PAD_BACK) {
        if (sPage == PAGE_CAPTURE) {
            sPage = PAGE_CONTROLS;
        } else {
            sClosing = 1;
        }
        return 1;
    }
    int step = (pressed & kDown) ? 1 : (pressed & kUp) ? -1 : 0;
    sHeldFor = (now & (kUp | kDown)) ? sHeldFor + 1 : 0;
    if (step == 0 && sHeldFor >= REPEAT_AFTER && (sHeldFor - REPEAT_AFTER) % REPEAT_EVERY == 0) {
        step = (now & kDown) ? 1 : -1;
    }
    switch (sPage) {
        case PAGE_MAIN:
            UpdateMain(pressed, step);
            break;
        case PAGE_CONTROLS:
            UpdateControls(pressed, step);
            break;
        case PAGE_CAPTURE:
            UpdateCapture(pressed);
            break;
        case PAGE_GROUPS:
            UpdateGroups(pressed, step);
            break;
        case PAGE_LIST:
            UpdateList(pressed, step);
            break;
        case PAGE_DEBUG:
            UpdateDebug(pressed, step);
            break;
        case PAGE_MODS:
            UpdateMods(pressed, step);
            break;
        case PAGE_CONFIRM:
            UpdateConfirm(pressed, step);
            break;
    }
    return 1;
}

// text with a soft drop shadow under it
static void Text(int font, float x, float y, const char* s, uint32_t colour, int align) {
    Ovl_Text(font, x + 2.0f, y + 3.0f, s, C_TEXT_SHADOW, align);
    Ovl_Text(font, x, y, s, colour, align);
}

static void RowBack(float y, float half, bool chosen) {
    if (chosen) {
        Ovl_RoundRect(ROW_X0 - 3.0f, y - half - 3.0f, ROW_X1 + 3.0f, y + half + 3.0f, 17.0f, C_GOLD_TOP, C_GOLD_BOTTOM);
        Ovl_RoundRect(ROW_X0, y - half, ROW_X1, y + half, 14.0f, C_CHOSEN_TOP, C_CHOSEN_BOTTOM);
    } else {
        Ovl_RoundRect(ROW_X0, y - half, ROW_X1, y + half, 14.0f, C_ROW, C_ROW);
    }
}

// a row of one centred label
static void Row(float y, float half, const char* label, bool chosen) {
    RowBack(y, half, chosen);
    Text(OVL_FONT_BODY, CENTRE_X, y, label, chosen ? C_CHOSEN_VALUE : C_LABEL, OVL_ALIGN_CENTRE);
}

// how far a label `over` too long has scrolled: a second at the start, along it, a second at the end, and again
static float ScrollBy(float over) {
    const int hold = SCROLL_HOLD, along = (int)(over / SCROLL_SPEED) + 1;
    const int at = sScrollFrames % (hold + along + hold);

    return at < hold ? 0.0f : at < hold + along ? (at - hold) * SCROLL_SPEED : over;
}

// a row of a label and its value; `arrows`, chosen: left and right change it, and it says so
static void ValueRow(float y, float half, const char* label, const char* value, bool chosen, bool arrows = false) {
    // what the label has, clear of the value and its arrows
    const float room = VALUE_X - LABEL_X - Ovl_TextWidth(OVL_FONT_SMALL, value) - ARROW_GAP * 3.0f;
    const float over = Ovl_TextWidth(OVL_FONT_BODY, label) - room;

    RowBack(y, half, chosen);
    if (over > 0.0f) {
        // too long: cut off there, and on the chosen row, scrolled along to its end and back
        Ovl_Clip(LABEL_X, LABEL_X + room);
        Text(OVL_FONT_BODY, LABEL_X - (chosen ? ScrollBy(over) : 0.0f), y, label, chosen ? C_CHOSEN : C_LABEL,
             OVL_ALIGN_LEFT);
        Ovl_ClipOff();
    } else {
        Text(OVL_FONT_BODY, LABEL_X, y, label, chosen ? C_CHOSEN : C_LABEL, OVL_ALIGN_LEFT);
    }
    Text(OVL_FONT_SMALL, VALUE_X, y, value, chosen ? C_CHOSEN_VALUE : C_VALUE, OVL_ALIGN_RIGHT);
    if (chosen && arrows) {
        const float l = VALUE_X - Ovl_TextWidth(OVL_FONT_SMALL, value) - ARROW_GAP, r = VALUE_X + ARROW_GAP;

        Ovl_Tri(l, y - ARROW_SIZE, l, y + ARROW_SIZE, l - ARROW_SIZE * 1.2f, y, C_GOLD_TOP);
        Ovl_Tri(r, y - ARROW_SIZE, r + ARROW_SIZE * 1.2f, y, r, y + ARROW_SIZE, C_GOLD_TOP);
    }
}

// a pad button as the pad has it, and what it does: returns the width used
static float Hint(float x, const char* badge, uint32_t badgeColour, const char* what) {
    const float w = BADGE_RADIUS * 2.0f + 8.0f;

    Ovl_Circle(x + BADGE_RADIUS, FOOTER_Y, BADGE_RADIUS, badgeColour);
    Ovl_Text(OVL_FONT_SMALL, x + BADGE_RADIUS, FOOTER_Y, badge, 0xFFFFFFFFu, OVL_ALIGN_CENTRE);
    Ovl_Text(OVL_FONT_SMALL, x + w, FOOTER_Y, what, C_HINT, OVL_ALIGN_LEFT);
    return w + Ovl_TextWidth(OVL_FONT_SMALL, what);
}

static float HintWidth(const char* what) {
    return BADGE_RADIUS * 2.0f + 8.0f + Ovl_TextWidth(OVL_FONT_SMALL, what);
}

// for a row left and right change: two arrows, A and X if they do something too, and B
static void FooterChange(const char* what = "Change", const char* a = NULL, const char* xButton = NULL) {
    static const float kGap = 28.0f, kArrows = 34.0f;
    float x = CENTRE_X - (kArrows + Ovl_TextWidth(OVL_FONT_SMALL, what) + kGap + (a ? HintWidth(a) + kGap : 0.0f) +
                          (xButton ? HintWidth(xButton) + kGap : 0.0f) + HintWidth("Back")) * 0.5f;

    if (a != NULL) {
        x += Hint(x, "A", C_BADGE_A, a) + kGap;
    }
    Ovl_Tri(x + 10.0f, FOOTER_Y - 8.0f, x + 10.0f, FOOTER_Y + 8.0f, x, FOOTER_Y, C_HINT);
    Ovl_Tri(x + 16.0f, FOOTER_Y - 8.0f, x + 26.0f, FOOTER_Y, x + 16.0f, FOOTER_Y + 8.0f, C_HINT);
    Ovl_Text(OVL_FONT_SMALL, x + kArrows, FOOTER_Y, what, C_HINT, OVL_ALIGN_LEFT);
    x += kArrows + Ovl_TextWidth(OVL_FONT_SMALL, what) + kGap;
    if (xButton != NULL) {
        x += Hint(x, "X", C_BADGE_X, xButton) + kGap;
    }
    Hint(x, "B", C_BADGE_B, "Back");
}

// on a mod of the Mods page's: A on or off, X and Y up and down, B
static void FooterMods(void) {
    static const float kGap = 30.0f;
    float x = CENTRE_X - (HintWidth("On / off") + HintWidth("Up") + HintWidth("Down") + HintWidth("Back") + kGap * 3.0f) * 0.5f;

    x += Hint(x, "A", C_BADGE_A, "On / off") + kGap;
    x += Hint(x, "X", C_BADGE_X, "Up") + kGap;
    x += Hint(x, "Y", C_BADGE_Y, "Down") + kGap;
    Hint(x, "B", C_BADGE_B, "Back");
}

static void Footer(const char* a, const char* x, const char* b) {
    static const float kGap = 34.0f;
    // measured first, to centre them
    float at = CENTRE_X - (HintWidth(a) + kGap + (x ? HintWidth(x) + kGap : 0.0f) + HintWidth(b)) * 0.5f;

    at += Hint(at, "A", C_BADGE_A, a) + kGap;
    if (x != NULL) {
        at += Hint(at, "X", C_BADGE_X, x) + kGap;
    }
    Hint(at, "B", C_BADGE_B, b);
}

static void Panel(const char* title) {
    // the game dimmed behind; the panel's shadow, gold rim and body
    Ovl_Rect(0.0f, 0.0f, OVL_W, OVL_H, C_DIM, C_DIM);
    Ovl_RoundRect(PANEL_X0 + 8.0f, PANEL_Y0 + 12.0f, PANEL_X1 + 8.0f, PANEL_Y1 + 12.0f, RADIUS, C_SHADOW, C_SHADOW);
    Ovl_RoundRect(PANEL_X0, PANEL_Y0, PANEL_X1, PANEL_Y1, RADIUS, C_GOLD_TOP, C_GOLD_BOTTOM);
    Ovl_RoundRect(PANEL_X0 + BORDER, PANEL_Y0 + BORDER, PANEL_X1 - BORDER, PANEL_Y1 - BORDER, RADIUS - BORDER,
                  C_PANEL_TOP, C_PANEL_BOTTOM);
    // the title's band: rounded at the top, square where it meets the body
    Ovl_RoundRect(PANEL_X0 + BORDER, PANEL_Y0 + BORDER, PANEL_X1 - BORDER, HEADER_Y1, RADIUS - BORDER, C_HEADER_TOP,
                  C_HEADER_BOTTOM);
    Ovl_Rect(PANEL_X0 + BORDER, HEADER_Y1 - RADIUS, PANEL_X1 - BORDER, HEADER_Y1, C_HEADER_MIDDLE, C_HEADER_BOTTOM);
    Ovl_Rect(PANEL_X0 + BORDER, HEADER_Y1, PANEL_X1 - BORDER, HEADER_Y1 + 3.0f, C_GOLD_TOP, C_GOLD_BOTTOM);
    Text(OVL_FONT_TITLE, CENTRE_X, TITLE_Y, title, C_TITLE, OVL_ALIGN_CENTRE);
}

static void DrawMain(void) {
    static const char* const kRows[ROW_COUNT] = { "Back to the game", "Controls", "Cheats",       "Options",
                                                  "Mods",             "Debug",    "Restart game", "Exit to dashboard" };

    Panel("Banjo-Kazooie-X");
    for (int i = 0; i < ROW_COUNT; i++) {
        Row(ROWS_Y + i * MAIN_STEP, ROW_HALF, kRows[i], sCursor == i);
    }
    Footer("Select", NULL, "Back");
}

// more of a list above or below; `first`, the first row that scrolls
static void MoreMarks(int rows, int first = 0) {
    if (sTop > first) {
        const float y = first > 0 ? LIST_Y + first * LIST_STEP - 20.0f : 168.0f;
        Ovl_Tri(CENTRE_X - 12.0f, y, CENTRE_X, y - 10.0f, CENTRE_X + 12.0f, y, C_GOLD_TOP);
    }
    if (sTop + LIST_ROWS - first < rows) {
        const float y = LIST_Y + LIST_ROWS * LIST_STEP - 20.0f;
        Ovl_Tri(CENTRE_X - 12.0f, y, CENTRE_X + 12.0f, y, CENTRE_X, y + 10.0f, C_GOLD_TOP);
    }
}

static void DrawDebug(void) {
    char text[32];
    float y = ROWS_Y;

    Panel("Debug");
    ValueRow(y, ROW_HALF, kDebugMode.label, OptionText(kDebugMode, text, sizeof(text)), sCursor == DEBUG_MODE, true);
    y += ROW_STEP;
    ValueRow(y, ROW_HALF, kLogging.label, OptionText(kLogging, text, sizeof(text)), sCursor == DEBUG_LOGGING, true);
    y += ROW_STEP;
#if LUSX_DEBUG_TOOLS
    ValueRow(y, ROW_HALF, kTestOverlay.label, OptionText(kTestOverlay, text, sizeof(text)), sCursor == DEBUG_OVERLAY,
             true);
    y += ROW_STEP;
#endif
    if (ShowDebugRows()) {
        // both want a save being played (port/lhx_save_edit.cpp)
        const bool played = LusxEdit_Available();

        if (played) {
            Row(y, ROW_HALF, "Warp", sCursor == DEBUG_WARP);
        } else {
            ValueRow(y, ROW_HALF, "Warp", "No save loaded", sCursor == DEBUG_WARP);
        }
        y += ROW_STEP;
        if (played) {
            Row(y, ROW_HALF, "Edit save", sCursor == DEBUG_EDIT);
        } else {
            ValueRow(y, ROW_HALF, "Edit save", "No save loaded", sCursor == DEBUG_EDIT);
        }
        y += ROW_STEP;
    }
    Row(y, ROW_HALF, "Back", sCursor == DEBUG_BACK);
    if (sCursor == DEBUG_MODE || sCursor == DEBUG_LOGGING) {
        FooterChange();
#if LUSX_DEBUG_TOOLS
    } else if (sCursor == DEBUG_OVERLAY) {
        FooterChange();
#endif
    } else {
        Footer("Select", NULL, "Back");
    }
}

static void DrawGroups(void) {
    Panel(sGroupsTitle);
    for (int i = 0; i < sGroupCount; i++) {
        Row(ROWS_Y + i * ROW_STEP, ROW_HALF, sGroups[i].title, sCursor == i);
    }
    Row(ROWS_Y + sGroupCount * ROW_STEP, ROW_HALF, "Back", sCursor == sGroupCount);
    Footer("Select", NULL, "Back");
}

static void DrawList(void) {
    const int rows = sListCount + 1;
    char text[16];

    Panel(sListTitle);
    for (int i = 0; i < LIST_ROWS; i++) {
        // the pinned rows where they are, then the rest from the top of the scroll
        const int row = i < sPinned ? i : sTop + i - sPinned;
        const float y = LIST_Y + i * LIST_STEP;
        const bool chosen = row == sCursor;

        if (row >= rows) {
            break;
        }
        if (row == sListCount) {
            Row(y, LIST_HALF, "Back", chosen);
        } else {
            ValueRow(y, LIST_HALF, sList[row].label, OptionText(sList[row], text, sizeof(text)), chosen,
                     sList[row].kind != OPT_ACTION);
        }
    }
    if (sPinned > 0) {
        const float line = LIST_Y + (sPinned - 0.5f) * LIST_STEP;
        Ovl_Rect(ROW_X0, line - 1.0f, ROW_X1, line + 1.0f, C_GOLD_TOP, C_GOLD_BOTTOM);
    }
    MoreMarks(rows, sPinned);
    if (sCursor == sListCount || sList[sCursor].kind == OPT_ACTION) {
        Footer("Select", NULL, "Back");
    } else if (sCursor < sPinned) {
        FooterChange("Change", NULL, "Areas");
    } else if (sList[sCursor].press != NULL) {
        FooterChange("Entrance", "Warp", sPinned > 0 ? "Settings" : NULL);
    } else {
        FooterChange();
    }
}

static void DrawControls(void) {
    char text[16];

    Panel("Controls");
    for (int i = 0; i < LIST_ROWS && sTop + i < LIST_COUNT; i++) {
        const int row = sTop + i;
        const float y = LIST_Y + i * LIST_STEP;
        const bool chosen = row == sCursor;

        if (row == LIST_RUMBLE) {
            ValueRow(y, LIST_HALF, kRumble.label, OptionText(kRumble, text, sizeof(text)), chosen, true);
        } else if (row == LIST_RESET) {
            Row(y, LIST_HALF, "Reset to defaults", chosen);
        } else if (row == LIST_BACK) {
            Row(y, LIST_HALF, "Back", chosen);
        } else if (chosen && sPage == PAGE_CAPTURE) {
            ValueRow(y, LIST_HALF, kButtons[row].name, "Press a button", true);
        } else {
            ValueRow(y, LIST_HALF, kButtons[row].name, sValues[row].c_str(), chosen);
        }
    }
    MoreMarks(LIST_COUNT);
    if (sPage == PAGE_CAPTURE) {
        Ovl_Text(OVL_FONT_SMALL, CENTRE_X, FOOTER_Y, sAdd ? "Press the button to add. Back cancels."
                                                          : "Press the new button. Back cancels.",
                 C_HINT, OVL_ALIGN_CENTRE);
    } else if (sCursor < BUTTON_COUNT) {
        Footer("Change", "Add another", "Back");
    } else if (sCursor == LIST_RUMBLE) {
        FooterChange();
    } else {
        Footer("Select", NULL, "Back");
    }
}

// a line of the Mods page's under its rows, cut off at the rows' edges
static void ModInfo(int line, const std::string& s, uint32_t colour) {
    Ovl_Clip(ROW_X0, ROW_X1);
    Text(OVL_FONT_SMALL, CENTRE_X, MOD_INFO_Y + line * MOD_INFO_STEP, s.c_str(), colour, OVL_ALIGN_CENTRE);
    Ovl_ClipOff();
}

static void DrawMods(void) {
    const std::vector<LusxMod>& mods = LusxMods_List();
    const int count = ModCount(), rows = ModRows(), restart = ModRestartRow();

    Panel("Mods");
    if (count == 0) {
        Text(OVL_FONT_BODY, CENTRE_X, LIST_Y, "No mods found.", C_LABEL, OVL_ALIGN_CENTRE);
        Text(OVL_FONT_SMALL, CENTRE_X, LIST_Y + LIST_STEP, "Put .o2r or .zip mods in the mods folder", C_HINT,
             OVL_ALIGN_CENTRE);
        Text(OVL_FONT_SMALL, CENTRE_X, LIST_Y + LIST_STEP * 1.7f, "beside default.xbe, then restart the game.", C_HINT,
             OVL_ALIGN_CENTRE);
        Row(LIST_Y + LIST_STEP * 3.0f, LIST_HALF, "Back", true);
        Footer("Select", NULL, "Back");
        return;
    }
    for (int i = 0; i < MOD_ROWS && sTop + i < rows; i++) {
        const int row = sTop + i;
        const float y = LIST_Y + i * LIST_STEP;
        const bool chosen = row == sCursor;

        if (row < count) {
            // its place, and its file's name as it is in the mods folder
            const LusxMod& mod = mods[row];
            const size_t dot = mod.file.find_last_of('.');
            const std::string label = std::to_string(row + 1) + ". " +
                                      (dot != std::string::npos && dot > 0 ? mod.file.substr(0, dot) : mod.file);
            ValueRow(y, LIST_HALF, label.c_str(), LusxMods_IsOff(mod.file) ? "Off" : "On", chosen, true);
        } else if (row == restart) {
            Row(y, LIST_HALF, "Restart to apply", chosen);
        } else {
            Row(y, LIST_HALF, "Back", chosen);
        }
    }
    // more above or below
    if (sTop > 0) {
        Ovl_Tri(CENTRE_X - 12.0f, 168.0f, CENTRE_X, 158.0f, CENTRE_X + 12.0f, 168.0f, C_GOLD_TOP);
    }
    if (sTop + MOD_ROWS < rows) {
        const float y = LIST_Y + MOD_ROWS * LIST_STEP - 20.0f;
        Ovl_Tri(CENTRE_X - 12.0f, y, CENTRE_X + 12.0f, y, CENTRE_X, y + 10.0f, C_GOLD_TOP);
    }

    // the chosen one: its file, and what's become of it
    if (sCursor < count) {
        const LusxMod& mod = mods[sCursor];
        const bool off = LusxMods_IsOff(mod.file);

        ModInfo(0, mod.file, C_VALUE);
        if (off != !mod.on) {
            ModInfo(1, off ? "Turned off: restart to apply" : "Turned on: restart to apply", C_CHOSEN_VALUE);
        } else if (ModMoved(sCursor)) {
            ModInfo(1, "Moved: restart to apply", C_CHOSEN_VALUE);
        } else if (off) {
            ModInfo(1, "Off: not loaded", C_HINT);
        } else if (!CVarGetInteger("gSettings.Mods.AlternateAssets", 0)) {
            // (a texture pack's textures are Lighthouse's alternate assets)
            ModInfo(1, "Loaded, but HD textures are off (Options, Display)", C_CHOSEN_VALUE);
        } else {
            ModInfo(1, "Loaded: wins over the ones below it", C_HINT);
        }
        FooterMods();
    } else {
        if (sCursor == restart) {
            ModInfo(0, "Starts the game again with the mods as set here.", C_HINT);
        }
        Footer("Select", NULL, "Back");
    }
}

static void DrawConfirm(void) {
    const bool restart = sConfirmRow == ROW_RESTART;

    Panel(restart ? "Restart game?" : "Exit to dashboard?");
    Text(OVL_FONT_BODY, CENTRE_X, ROWS_Y + 20.0f, "Anything not saved will be lost.", C_LABEL, OVL_ALIGN_CENTRE);
    Row(ROWS_Y + ROW_STEP * 2.0f, ROW_HALF, "No, go back", sCursor == 0);
    Row(ROWS_Y + ROW_STEP * 3.0f, ROW_HALF, restart ? "Yes, restart" : "Yes, exit", sCursor == 1);
    Footer("Select", NULL, "Back");
}

// The Test overlay (the Debug page): lusx_debug.cpp's lines over the game. Ocarina-X draws them through the game's own
// debug printer; Banjo-Kazooie has none to hand, so they're drawn here, after the frame's replay, in the menu's small
// font, on a dark panel. Its fonts stay loaded while the overlay shows (under 1 MB: a tester's tool, off by default);
// the menu's closing lets them go, and turning the overlay off is done from the menu.
bool LusxMenu_DrawTestOverlay(Fast::GfxRenderingAPI* rapi) {
#if LUSX_DEBUG_TOOLS
    static const float kX = 24.0f, kY = 20.0f, kLine = 28.0f, kPad = 10.0f;
    int lines = 0;
    float width = 0.0f;

    if (sOpen || LusxDebug_OverlayLine(0) == NULL || !Ovl_Load(rapi)) {
        return false;
    }
    for (; LusxDebug_OverlayLine(lines) != NULL; lines++) {
        const float w = Ovl_TextWidth(OVL_FONT_SMALL, LusxDebug_OverlayLine(lines));
        width = w > width ? w : width;
    }
    Ovl_Begin();
    Ovl_RoundRect(kX - kPad, kY - kPad, kX + width + kPad, kY + lines * kLine + kPad, 8.0f, 0xA0000000u, 0xA0000000u);
    for (int i = 0; i < lines; i++) {
        Text(OVL_FONT_SMALL, kX, kY + (i + 0.5f) * kLine, LusxDebug_OverlayLine(i), 0xFFFFFF50u, OVL_ALIGN_LEFT);
    }
    return true;
#else
    (void)rapi;
    return false;
#endif
}

void LusxMenu_Draw(Fast::GfxRenderingAPI* rapi) {
    static int sLastCursor;
    static Page sLastPage;

    if (sClosing) {
        return;
    }
    sScrollFrames = sCursor == sLastCursor && sPage == sLastPage ? sScrollFrames + 1 : 0;
    sLastCursor = sCursor;
    sLastPage = sPage;
    Ovl_Begin();
    switch (sPage) {
        case PAGE_MAIN:
            DrawMain();
            break;
        case PAGE_CONTROLS:
        case PAGE_CAPTURE:
            DrawControls();
            break;
        case PAGE_GROUPS:
            DrawGroups();
            break;
        case PAGE_LIST:
            DrawList();
            break;
        case PAGE_DEBUG:
            DrawDebug();
            break;
        case PAGE_MODS:
            DrawMods();
            break;
        case PAGE_CONFIRM:
            DrawConfirm();
            break;
    }
}
