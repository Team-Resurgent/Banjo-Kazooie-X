// lhx_save_edit.cpp -- Banjo-Kazooie-X: the port's menu's Debug tools, Warp and Edit save, as in Majora's-X and
// Ocarina-X (xbox/src/lusx_menu.cpp draws them; lusx_options.h). Lighthouse has both as ImGui windows, left out of
// this build with the rest of its GUI (src/port/UI/DeveloperTools/Warps.cpp and SaveEditor.cpp); here are their lists
// and their changes, made the way those windows make them.
//
// The game stands still while the menu is open (its tick waits on the frame the menu is drawn over), so a count or a
// flag is changed in place and the menu shows it at once. What reaches into the running game, a warp, an actor
// despawned or reset, the HUD shown, goes to the game's own thread (Lighthouse::RunOnGameThread), as the PC windows
// send it, and is done as the next tick starts.
#include "lusx_options.h"

#include <stdio.h>

#include <libultraship/bridge/consolevariablebridge.h>

#include "port/Enhancements/Events/Hooks/Events.h"
#include "port/Enhancements/Retention/Retention.h"
#include "port/FrameHost.h"
#include "port/Save/Types.h"
#include "port/ShipInit.hpp"
#include "port/ShipUtils.h"
#include "port/UI/DeveloperTools/MapNames.h"

extern "C" {
#include "functions.h"
#include "prop.h"
#include "core2/abilityprogress.h"
#include "core2/fileprogress.h"
#include "core2/gsworld.h"

bool player_isAbilityUnlocked(enum ability_e uid);
extern u8 sItemscoreNoteScores[0xE];
extern ActorArray* suBaddieActorArray;
}

#define ROW(label, arg, step, text) \
    { OPT_CUSTOM, label, NULL, 0, NULL, NULL, 0, -1, NULL, arg, step, text }
#define ACTION(label, step) \
    { OPT_ACTION, label, NULL, 0, NULL, NULL, 0, -1, NULL, 0, step, Select }
#define LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

#define DEFAULT_MAX_HEALTH 8 // as SaveEditor.cpp's

static int Clamp(int v, int lowest, int highest) {
    return v < lowest ? lowest : v > highest ? highest : v;
}

static int Wrap(int v, int lowest, int highest) {
    return v < lowest ? highest : v > highest ? lowest : v;
}

static const char* Number(char* buffer, size_t size, int n) {
    snprintf(buffer, size, "%d", n);
    return buffer;
}

static const char* OnOff(bool on) {
    return on ? "On" : "Off";
}

static const char* Select(int, char*, size_t) {
    return "Press A";
}

// a file being played: from its start on the file select (OnGameLoad) to the file select again. Not the attract demos.
static bool sFilePlayed;

static void ListenForFiles(void) {
    REGISTER_LISTENER(OnGameLoad, EVENT_PRIORITY_NORMAL, [](IEvent*) { sFilePlayed = true; });
    REGISTER_LISTENER(OnLoadFileSelect, EVENT_PRIORITY_NORMAL, [](IEvent*) { sFilePlayed = false; });
}

static RegisterShipInitFunc sListenForFiles(ListenForFiles);

static void NameMoves(void);

bool LusxEdit_Available(void) {
    NameMoves(); // (asked before the page opens and as its row is drawn)
    return sFilePlayed && gsworld_getMap() != MAP_91_FILE_SELECT;
}

// the worlds as the PC editor lists them, by their names in the game
static const enum level_e kLevelOrder[] = {
    LEVEL_B_SPIRAL_MOUNTAIN,     LEVEL_6_LAIR,           LEVEL_1_MUMBOS_MOUNTAIN, LEVEL_2_TREASURE_TROVE_COVE,
    LEVEL_3_CLANKERS_CAVERN,     LEVEL_4_BUBBLEGLOOP_SWAMP, LEVEL_5_FREEZEEZY_PEAK, LEVEL_7_GOBIS_VALLEY,
    LEVEL_A_MAD_MONSTER_MANSION, LEVEL_9_RUSTY_BUCKET_BAY, LEVEL_8_CLICK_CLOCK_WOOD,
};

static const char* WorldName(int level) {
    static const char* const kNames[] = {
        "",
        "Mumbo's Mountain",
        "Treasure Trove Cove",
        "Clanker's Cavern",
        "Bubblegloop Swamp",
        "Freezeezy Peak",
        "Gruntilda's Lair",
        "Gobi's Valley",
        "Click Clock Wood",
        "Rusty Bucket Bay",
        "Mad Monster Mansion",
        "Spiral Mountain",
    };
    return level > 0 && level < LEN(kNames) ? kNames[level] : "?";
}

static const WorldDef& World(int level) {
    for (const WorldDef& world : kWorlds) {
        if (world.levelId == level) {
            return world;
        }
    }
    return kWorlds[kWorldCount - 1];
}

// ---- General: health, lives and ammo (SaveEditor.cpp's General tab), and Gameplay Tools' two grants

static void StepHealth(int, int direction) {
    item_set(ITEM_14_HEALTH, Clamp(item_getCount(ITEM_14_HEALTH) + direction, 1, item_getCount(ITEM_15_HEALTH_TOTAL)));
}

static const char* TextHealth(int, char* buffer, size_t size) {
    return Number(buffer, size, item_getCount(ITEM_14_HEALTH));
}

static void StepMaxHealth(int, int direction) {
    const int most = fileProgressFlag_get(FILEPROG_B9_DOUBLE_HEALTH) ? DEFAULT_MAX_HEALTH * 2 : DEFAULT_MAX_HEALTH;

    item_set(ITEM_15_HEALTH_TOTAL, Clamp(item_getCount(ITEM_15_HEALTH_TOTAL) + direction, 1, most));
    item_set(ITEM_14_HEALTH, Clamp(item_getCount(ITEM_14_HEALTH), 1, item_getCount(ITEM_15_HEALTH_TOTAL)));
}

static const char* TextMaxHealth(int, char* buffer, size_t size) {
    return Number(buffer, size, item_getCount(ITEM_15_HEALTH_TOTAL));
}

static void StepDoubleHealth(int, int) {
    const bool on = !fileProgressFlag_get(FILEPROG_B9_DOUBLE_HEALTH);

    fileProgressFlag_set(FILEPROG_B9_DOUBLE_HEALTH, on);
    if (!on) {
        item_set(ITEM_15_HEALTH_TOTAL, Clamp(item_getCount(ITEM_15_HEALTH_TOTAL), 1, DEFAULT_MAX_HEALTH));
        item_set(ITEM_14_HEALTH, Clamp(item_getCount(ITEM_14_HEALTH), 1, item_getCount(ITEM_15_HEALTH_TOTAL)));
    }
}

static const char* TextDoubleHealth(int, char*, size_t) {
    return OnOff(fileProgressFlag_get(FILEPROG_B9_DOUBLE_HEALTH));
}

static void StepLives(int, int direction) {
    item_set(ITEM_16_LIFE, Clamp(item_getCount(ITEM_16_LIFE) + direction, 0, 99));
}

static const char* TextLives(int, char* buffer, size_t size) {
    return Number(buffer, size, item_getCount(ITEM_16_LIFE));
}

// the ammo: `arg` is which, its Cheato flag (Cheato's double bags) and its bag's size without
static const struct {
    enum item_e item;
    enum file_progress_e cheato;
    int limit, step;
} kAmmo[] = {
    { ITEM_D_EGGS, FILEPROG_BE_CHEATO_BLUEEGGS, 100, 10 },
    { ITEM_F_RED_FEATHER, FILEPROG_BF_CHEATO_REDFEATHERS, 50, 5 },
    { ITEM_10_GOLD_FEATHER, FILEPROG_C0_CHEATO_GOLDFEATHERS, 10, 1 },
};

static int AmmoLimit(int ammo) {
    return fileProgressFlag_get(kAmmo[ammo].cheato) ? kAmmo[ammo].limit * 2 : kAmmo[ammo].limit;
}

static void StepAmmo(int ammo, int direction) {
    item_set(kAmmo[ammo].item,
             Clamp(item_getCount(kAmmo[ammo].item) + direction * kAmmo[ammo].step, 0, AmmoLimit(ammo)));
}

static const char* TextAmmo(int ammo, char* buffer, size_t size) {
    return Number(buffer, size, item_getCount(kAmmo[ammo].item));
}

static void StepCheato(int ammo, int) {
    const bool on = !fileProgressFlag_get(kAmmo[ammo].cheato);

    fileProgressFlag_set(kAmmo[ammo].cheato, on);
    if (!on && item_getCount(kAmmo[ammo].item) > kAmmo[ammo].limit) {
        item_set(kAmmo[ammo].item, kAmmo[ammo].limit);
    }
}

static const char* TextCheato(int ammo, char*, size_t) {
    return OnOff(fileProgressFlag_get(kAmmo[ammo].cheato));
}

static void UnlockAllMoves(int, int) {
    for (int i = ABILITY_0_BARGE; i <= ABILITY_12_WONDERWING; i++) {
        ability_setLearned((enum ability_e)i, true);
    }
    ability_setAllUsed(-1);
}

static void FillConsumables(int, int) {
    for (int i = 0; i < LEN(kAmmo); i++) {
        item_set(kAmmo[i].item, AmmoLimit(i));
    }
    item_set(ITEM_1C_MUMBO_TOKEN, 25); // as Gameplay Tools' Add All Consumables
}

static const Option kGeneral[] = {
    ROW("Health", 0, StepHealth, TextHealth),
    ROW("Max health", 0, StepMaxHealth, TextMaxHealth),
    ROW("Double health", 0, StepDoubleHealth, TextDoubleHealth),
    ROW("Lives", 0, StepLives, TextLives),
    ROW("Eggs", 0, StepAmmo, TextAmmo),
    ROW("Red feathers", 1, StepAmmo, TextAmmo),
    ROW("Gold feathers", 2, StepAmmo, TextAmmo),
    ROW("Cheato: double eggs", 0, StepCheato, TextCheato),
    ROW("Cheato: double red feathers", 1, StepCheato, TextCheato),
    ROW("Cheato: double gold feathers", 2, StepCheato, TextCheato),
    ACTION("Fill eggs, feathers and tokens", FillConsumables),
    ACTION("Learn every move", UnlockAllMoves),
};

// ---- Moves: SaveEditor.cpp's Ability Unlocks, by the names the game's tutorials give them

static void StepMove(int ability, int) {
    ability_setLearned((enum ability_e)ability, !player_isAbilityUnlocked((enum ability_e)ability));
}

static const char* TextMove(int ability, char*, size_t) {
    return OnOff(player_isAbilityUnlocked((enum ability_e)ability));
}

static Option sMoves[ABILITY_12_WONDERWING + 1];

// ---- Unlocks: the worlds' doors and the warp cauldrons (SaveEditor.cpp's World and Cauldron Unlocks)

// the doors, in FILEPROG_31_MM_OPEN's order: told to set themselves up again, so an open door opens where Banjo is
static const enum actor_e kWorldDoors[] = {
    ACTOR_20E_MM_ENTRANCE_DOOR,  ACTOR_211_TTC_ENTRANCE_CHEST_LID, ACTOR_212_CC_ENTRANCE_BARS,
    ACTOR_210_BGS_ENTRANCE_DOOR, ACTOR_235_FP_ENTRANCE_DOOR_LEFT,  ACTOR_226_GV_ENTRANCE,
    ACTOR_228_MMM_ENTRANCE_DOOR, ACTOR_20F_RBB_ENTRANCE_DOOR,      ACTOR_234_CCW_ENTRANCE_DOOR,
};

static void StepWorldOpen(int flag, int) {
    fileProgressFlag_set((enum file_progress_e)flag, !fileProgressFlag_get((enum file_progress_e)flag));
    Lighthouse::RunOnGameThread([flag] {
        if (suBaddieActorArray != nullptr) {
            Actor* door = actorArray_findActorFromActorId(kWorldDoors[flag - FILEPROG_31_MM_OPEN]);
            if (door != nullptr) {
                door->volatile_initialized = false;
            }
        }
    });
}

static void StepFlag(int flag, int) {
    fileProgressFlag_set((enum file_progress_e)flag, !fileProgressFlag_get((enum file_progress_e)flag));
}

static const char* TextFlag(int flag, char*, size_t) {
    return OnOff(fileProgressFlag_get((enum file_progress_e)flag));
}

static const Option kUnlocks[] = {
    ROW("Mumbo's Mountain open", FILEPROG_31_MM_OPEN, StepWorldOpen, TextFlag),
    ROW("Treasure Trove Cove open", FILEPROG_32_TTC_OPEN, StepWorldOpen, TextFlag),
    ROW("Clanker's Cavern open", FILEPROG_33_CC_OPEN, StepWorldOpen, TextFlag),
    ROW("Bubblegloop Swamp open", FILEPROG_34_BGS_OPEN, StepWorldOpen, TextFlag),
    ROW("Freezeezy Peak open", FILEPROG_35_FP_OPEN, StepWorldOpen, TextFlag),
    ROW("Gobi's Valley open", FILEPROG_36_GV_OPEN, StepWorldOpen, TextFlag),
    ROW("Mad Monster Mansion open", FILEPROG_37_MMM_OPEN, StepWorldOpen, TextFlag),
    ROW("Rusty Bucket Bay open", FILEPROG_38_RBB_OPEN, StepWorldOpen, TextFlag),
    ROW("Click Clock Wood open", FILEPROG_39_CCW_OPEN, StepWorldOpen, TextFlag),
    ROW("Lower pink cauldron", FILEPROG_49_PINK_CAULDRON_1_ACTIVE, StepFlag, TextFlag),
    ROW("Upper pink cauldron", FILEPROG_4A_PINK_CAULDRON_2_ACTIVE, StepFlag, TextFlag),
    ROW("Lower green cauldron", FILEPROG_4B_GREEN_CAULDRON_1_ACTIVE, StepFlag, TextFlag),
    ROW("Upper green cauldron", FILEPROG_4C_GREEN_CAULDRON_2_ACTIVE, StepFlag, TextFlag),
    ROW("Lower red cauldron", FILEPROG_4D_RED_CAULDRON_1_ACTIVE, StepFlag, TextFlag),
    ROW("Upper red cauldron", FILEPROG_4E_RED_CAULDRON_2_ACTIVE, StepFlag, TextFlag),
    ROW("Lower yellow cauldron", FILEPROG_51_YELLOW_CAULDRON_1_ACTIVE, StepFlag, TextFlag),
    ROW("Upper yellow cauldron", FILEPROG_52_YELLOW_CAULDRON_2_ACTIVE, StepFlag, TextFlag),
};

// ---- Jiggies, notes, Mumbo tokens, honeycombs: SaveEditor.cpp's World Progress, a count a row. Right collects the
// world's next one still to get, left gives back the last one got, each as the PC editor's box for it would.

static const char* TextOf(char* buffer, size_t size, int got, int of) {
    snprintf(buffer, size, "%d of %d", got, of);
    return buffer;
}

// un-collecting a Jiggy of the world Banjo is in has the PC editor start the world again, to put it back; one restart
// for however many are given back while the menu is open
static bool sRestartWorld;

static void StepJiggies(int level, int direction) {
    const WorldDef& w = World(level);
    const int total = item_getCount(ITEM_26_JIGGY_TOTAL);

    if (direction > 0) {
        for (int i = w.jiggyStart; i < w.jiggyStart + w.jiggyCount; i++) {
            if (!jiggyscore_isCollected((enum jiggy_e)i)) {
                jiggyscore_setCollected(i, true);
                func_8034798C();
                item_set(ITEM_26_JIGGY_TOTAL, total + 1);
                Lighthouse::RunOnGameThread([i] {
                    ActorMarker* m = func_8032B16C((enum jiggy_e)i);
                    if (m != nullptr) {
                        marker_despawn(m);
                    }
                });
                return;
            }
        }
        return;
    }
    for (int i = w.jiggyStart + w.jiggyCount - 1; i >= w.jiggyStart; i--) {
        if (jiggyscore_isCollected((enum jiggy_e)i)) {
            jiggyscore_setCollected(i, false);
            func_8034798C();
            item_set(ITEM_26_JIGGY_TOTAL, total - 1);
            if (level == (int)level_get() && !sRestartWorld) {
                sRestartWorld = true;
                Lighthouse::RunOnGameThread([] {
                    sRestartWorld = false;
                    game_transitionToMapResettingLevel(gsworld_getMap(), gsworld_getExit(), 1);
                });
            }
            return;
        }
    }
}

static const char* TextJiggies(int level, char* buffer, size_t size) {
    const WorldDef& w = World(level);
    int got = 0;

    for (int i = w.jiggyStart; i < w.jiggyStart + w.jiggyCount; i++) {
        got += jiggyscore_isCollected((enum jiggy_e)i) != 0;
    }
    return TextOf(buffer, size, got, w.jiggyCount);
}

static void StepNotes(int level, int) {
    const bool all = itemscore_noteScores_get((enum level_e)level) < 100;

    sItemscoreNoteScores[level] = all ? 100 : 0;
    retention::setLevelNotesCollected(level, all);
}

static const char* TextNotes(int level, char* buffer, size_t size) {
    return TextOf(buffer, size, itemscore_noteScores_get((enum level_e)level), 100);
}

static void StepTokens(int level, int direction) {
    const WorldDef& w = World(level);
    const int held = item_getCount(ITEM_1C_MUMBO_TOKEN);

    if (direction > 0) {
        for (int i = w.mumboStart; i < w.mumboStart + w.mumboCount; i++) {
            if (!mumboscore_get((enum mumbotoken_e)i)) {
                mumboscore_set((enum mumbotoken_e)i, true);
                item_set(ITEM_1C_MUMBO_TOKEN, held + 1);
                return;
            }
        }
        return;
    }
    for (int i = w.mumboStart + w.mumboCount - 1; i >= w.mumboStart; i--) {
        if (mumboscore_get((enum mumbotoken_e)i)) {
            mumboscore_set((enum mumbotoken_e)i, false);
            item_set(ITEM_1C_MUMBO_TOKEN, held - 1);
            return;
        }
    }
}

static const char* TextTokens(int level, char* buffer, size_t size) {
    const WorldDef& w = World(level);
    int got = 0;

    for (int i = w.mumboStart; i < w.mumboStart + w.mumboCount; i++) {
        got += mumboscore_get((enum mumbotoken_e)i);
    }
    return TextOf(buffer, size, got, w.mumboCount);
}

// as SaveEditor.cpp's SaveEditor_SetHoneycomb: the most health follows, and a gain fills it
static void SetHoneycomb(int comb, bool collected) {
    const s32 healthTotal = item_getCount(ITEM_15_HEALTH_TOTAL);

    honeycombscore_set((enum honeycomb_e)comb, collected);
    func_8034789C();
    const bool gained = item_getCount(ITEM_15_HEALTH_TOTAL) > healthTotal;
    item_adjustByDiffWithoutHud(ITEM_14_HEALTH,
                                gained ? item_getCount(ITEM_15_HEALTH_TOTAL) - item_getCount(ITEM_14_HEALTH) : 0);
    Lighthouse::RunOnGameThread([] {
        item_adjustByDiffWithHud(ITEM_13_EMPTY_HONEYCOMB, 0);
        item_adjustByDiffWithHud(ITEM_14_HEALTH, 0);
    });
}

static void StepCombs(int level, int direction) {
    const WorldDef& w = World(level);

    if (direction > 0) {
        for (int i = w.honeycombStart; i < w.honeycombStart + w.honeycombCount; i++) {
            if (!honeycombscore_get((enum honeycomb_e)i)) {
                SetHoneycomb(i, true);
                return;
            }
        }
        return;
    }
    for (int i = w.honeycombStart + w.honeycombCount - 1; i >= w.honeycombStart; i--) {
        if (honeycombscore_get((enum honeycomb_e)i)) {
            SetHoneycomb(i, false);
            return;
        }
    }
}

static const char* TextCombs(int level, char* buffer, size_t size) {
    const WorldDef& w = World(level);
    int got = 0;

    for (int i = w.honeycombStart; i < w.honeycombStart + w.honeycombCount; i++) {
        got += honeycombscore_get((enum honeycomb_e)i);
    }
    return TextOf(buffer, size, got, w.honeycombCount);
}

// the per-world rows, made once: a row for each world that has any
static Option sJiggies[LEN(kLevelOrder)], sNotes[LEN(kLevelOrder)], sTokensCombs[LEN(kLevelOrder) * 2];
static char sTokenLabels[LEN(kLevelOrder)][40], sCombLabels[LEN(kLevelOrder)][40];
static int sJiggyCount, sNoteCount, sTokenCombCount;

static Option WorldRow(const char* label, int level, void (*step)(int, int), const char* (*text)(int, char*, size_t)) {
    Option o = ROW(label, level, step, text);
    return o;
}

// the moves' names are Lighthouse's (ShipUtils.cpp's abilityNameList, a std::vector made at start-up in another file):
// taken when the page is first wanted, not at start-up, where the order the two are made in is not known
static void NameMoves(void) {
    for (int i = ABILITY_0_BARGE; i <= ABILITY_12_WONDERWING; i++) {
        if (sMoves[i].label == NULL) {
            sMoves[i].label = i < (int)abilityNameList.size() ? abilityNameList[i].c_str() : "?";
        }
    }
}

// (the per-world pages' lengths are set as their rows are made, below)
extern OptionGroup kEditGroups[] = {
    { "General", kGeneral, LEN(kGeneral) },
    { "Moves", sMoves, LEN(sMoves) },
    { "Worlds and cauldrons", kUnlocks, LEN(kUnlocks) },
    { "Jiggies", sJiggies, 0 },
    { "Notes", sNotes, 0 },
    { "Mumbo tokens and honeycombs", sTokensCombs, 0 },
};
extern const int kEditGroupCount = LEN(kEditGroups);

static struct EditRows {
    EditRows() {
        for (int i = ABILITY_0_BARGE; i <= ABILITY_12_WONDERWING; i++) {
            sMoves[i] = WorldRow(NULL, i, StepMove, TextMove); // (its label: NameMoves)
        }
        for (int i = 0; i < LEN(kLevelOrder); i++) {
            const int level = kLevelOrder[i];
            const WorldDef& w = World(level);

            if (w.jiggyCount > 0) {
                sJiggies[sJiggyCount++] = WorldRow(WorldName(level), level, StepJiggies, TextJiggies);
            }
            if (w.hasNoteScore) {
                sNotes[sNoteCount++] = WorldRow(WorldName(level), level, StepNotes, TextNotes);
            }
        }
        for (int i = 0; i < LEN(kLevelOrder); i++) {
            const int level = kLevelOrder[i];
            const WorldDef& w = World(level);

            if (w.mumboCount > 0) {
                snprintf(sTokenLabels[i], sizeof(sTokenLabels[i]), "%s tokens", WorldName(level));
                sTokensCombs[sTokenCombCount++] = WorldRow(sTokenLabels[i], level, StepTokens, TextTokens);
            }
        }
        for (int i = 0; i < LEN(kLevelOrder); i++) {
            const int level = kLevelOrder[i];
            const WorldDef& w = World(level);

            if (w.honeycombCount > 0) {
                snprintf(sCombLabels[i], sizeof(sCombLabels[i]), "%s honeycombs", WorldName(level));
                sTokensCombs[sTokenCombCount++] = WorldRow(sCombLabels[i], level, StepCombs, TextCombs);
            }
        }
        kEditGroups[3].count = sJiggyCount;
        kEditGroups[4].count = sNoteCount;
        kEditGroups[5].count = sTokenCombCount;
    }
} sEditRows;

// ---- Warp: Lighthouse's Warps window's destinations (Warps.cpp's warpsInfo, in its order, by world), an area a row
// and its entrances on left and right; A warps there as the window's button does (func_8031D04C, the game's own
// warp), once the menu has closed.

struct WarpEntry {
    int32_t map;
    int32_t exit;
    const char* name;
};

// clang-format off
// Warps.cpp's warpsInfo, unchanged, and the 100% ending at the end
static const WarpEntry kWarps[] = {
    // LEVEL_1_MUMBOS_MOUNTAIN
    { MAP_2_MM_MUMBOS_MOUNTAIN, WARP_MM_1_MUMBOS_HUT,           "Outside Mumbo's Hut" },
    { MAP_2_MM_MUMBOS_MOUNTAIN, WARP_MM_2_TOWER_BOTTOM,         "Outside Ticker's Bottom" },
    { MAP_2_MM_MUMBOS_MOUNTAIN, WARP_MM_3_TOWER_TOP,            "Outside Ticker's Top" },
    { MAP_2_MM_MUMBOS_MOUNTAIN, WARP_MM_4_WITCH_SWITCH,         "Witch Switch" },
    { MAP_2_MM_MUMBOS_MOUNTAIN, WARP_MM_5_WORLD_ENTRACE,        "Warp Pad" },
    { MAP_2_MM_MUMBOS_MOUNTAIN, 91,                             "Demo" },
    { MAP_C_MM_TICKERS_TOWER,   WARP_MM_TICKERS_TOWER_1_TOP,    "Top" },
    { MAP_C_MM_TICKERS_TOWER,   WARP_MM_TICKERS_TOWER_2_BOTTOM, "Bottom" },
    { MAP_E_MM_MUMBOS_SKULL,    WARP_MM_MUMBOS_HUT_1_ENTRANCE,  "Entrance" },
    // LEVEL_2_TREASURE_TROVE_COVE
    { MAP_5_TTC_BLUBBERS_SHIP,       WARP_TCC_BLUBBERS_SHIP_5_TOP_HATCH,   "Crate" },
    { MAP_5_TTC_BLUBBERS_SHIP,       WARP_TCC_BLUBBERS_SHIP_6_SIDE_HATCH,  "Underwater" },
    { MAP_6_TTC_NIPPERS_SHELL,       WARP_TCC_NIPPERS_SHELL_1_ENTRANCE,    "Entrance" },
    { MAP_7_TTC_TREASURE_TROVE_COVE, WARP_TTC_1_LEAKY,                     "Leaky Cutscene Return" },
    { MAP_7_TTC_TREASURE_TROVE_COVE, WARP_TTC_3_SANDCASTLE,                "Outside Sandcastle" },
    { MAP_7_TTC_TREASURE_TROVE_COVE, WARP_TTC_4_WORLD_ENTRANCE,            "Warp Pad" },
    { MAP_7_TTC_TREASURE_TROVE_COVE, WARP_TTC_6_SHIP_TOP_HATCH,            "Ship Deck" },
    { MAP_7_TTC_TREASURE_TROVE_COVE, WARP_TTC_7_SHIP_SIDE_HATCH,           "Ship Pool" },
    { MAP_7_TTC_TREASURE_TROVE_COVE, WARP_TTC_8_LIGHTHOUSE_TOP,            "Lighthouse Top" },
    { MAP_7_TTC_TREASURE_TROVE_COVE, WARP_TTC_A_NIPPER,                    "Nipper" },
    { MAP_7_TTC_TREASURE_TROVE_COVE, WARP_TTC_C_LIGHTHOUSE_BOTTOM,         "Lighthouse Bottom" },
    { MAP_7_TTC_TREASURE_TROVE_COVE, WARP_TTC_E_ISLAND_TOP_STAIRS,         "Heights" },
    { MAP_7_TTC_TREASURE_TROVE_COVE, WARP_TTC_F_CLIFFSIDE_STAIRS,          "Cave" },
    { MAP_7_TTC_TREASURE_TROVE_COVE, WARP_TTC_14_WITCH_SWITCH,             "Witch Switch" },
    { MAP_7_TTC_TREASURE_TROVE_COVE, 91,                                   "Demo" },
    { MAP_7_TTC_TREASURE_TROVE_COVE, 95,                                   "Sharkfood SNS Cutscene" },
    { MAP_7_TTC_TREASURE_TROVE_COVE, 128,                                  "Outside Sharkfood" },
    { MAP_8F_TTC_SHARKFOOD_ISLAND,   WARP_TCC_SHARKFOOD_ISLAND_1_ENTRANCE, "Entrance" },
    { MAP_A_TTC_SANDCASTLE,          WARP_TCC_SANDCASTLE_1_ENTRANCE,       "Entrance" },
    { MAP_A_TTC_SANDCASTLE,          2,                                    "FF Dark Snippet Fight" },
    // LEVEL_3_CLANKERS_CAVERN
    { MAP_21_CC_WITCH_SWITCH_ROOM, WARP_CC_WITCH_SWITCH_1_TOP_ENTRANCE,            "From Blowhole" },
    { MAP_21_CC_WITCH_SWITCH_ROOM, WARP_CC_WITCH_SWITCH_14_WITCH_SWITCH,           "Witch Switch" },
    { MAP_22_CC_INSIDE_CLANKER,    WARP_CC_INSIDE_CLANKER_1_TOP,       "Stomach Ceiling" },
    { MAP_22_CC_INSIDE_CLANKER,    WARP_CC_INSIDE_CLANKER_2_MOUTH_ROOFTOP,         "Mouth Ceiling" },
    { MAP_22_CC_INSIDE_CLANKER,    WARP_CC_INSIDE_CLANKER_3_GOLD_FEATHER_ENTRANCE, "Top" },
    { MAP_22_CC_INSIDE_CLANKER,    WARP_CC_INSIDE_CLANKER_4_LEFT_GILL,             "Lower Gill" },
    { MAP_22_CC_INSIDE_CLANKER,    WARP_CC_INSIDE_CLANKER_5_RIGHT_GILL,            "Upper Gill" },
    { MAP_22_CC_INSIDE_CLANKER,    WARP_CC_INSIDE_CLANKER_6_LEFT_TOOTH,            "Jiggy Tooth" },
    { MAP_22_CC_INSIDE_CLANKER,    WARP_CC_INSIDE_CLANKER_7_RIGHT_TOOTH,           "Token Tooth" },
    { MAP_22_CC_INSIDE_CLANKER,    91,                                             "Mouth Ceiling Demo" },
    { MAP_23_CC_GOLDFEATHER_ROOM,  WARP_CC_GOLD_FEATHER_1_ENTRANCE,                "Wonder Wing Room" },
    { MAP_B_CC_CLANKERS_CAVERN,    WARP_CC_1_RIGHT_TOOTH,                          "Left Tooth" },
    { MAP_B_CC_CLANKERS_CAVERN,    WARP_CC_2_LEFT_TOOTH,                           "Right Tooth" },
    { MAP_B_CC_CLANKERS_CAVERN,    WARP_CC_3_RIGHT_GILLS,                          "Left Gills" },
    { MAP_B_CC_CLANKERS_CAVERN,    WARP_CC_4_LEFT_GILLS,                           "Right Gills" },
    { MAP_B_CC_CLANKERS_CAVERN,    WARP_CC_5_WORLD_ENTRANCE,                       "CC Warp Pad" },
    { MAP_B_CC_CLANKERS_CAVERN,    92,                                             "FF Mutie Fight" },
    // LEVEL_4_BUBBLEGLOOP_SWAMP
    { MAP_10_BGS_MR_VILE,          WARP_BGS_MR_VILE_2_CENTER,         "Minigame Finish" },
    { MAP_10_BGS_MR_VILE,          WARP_BGS_MR_VILE_3_LEFT_ENTRANCE,  "Right Nostril Tunnel" },
    { MAP_10_BGS_MR_VILE,          WARP_BGS_MR_VILE_4_RIGHT_ENTRANCE, "Left Nostril Tunnel" },
    { MAP_11_BGS_TIPTUP,           WARP_BGS_TIPTUP_1_ENTRANCE,        "Inside Tanktup" },
    { MAP_11_BGS_TIPTUP,           WARP_BGS_TIPTUP_2_PODIUM,          "Minigame Win Return" },
    { MAP_11_BGS_TIPTUP,           92,                                "FF Tiptup Minigame" },
    { MAP_47_BGS_MUMBOS_SKULL,     WARP_BGS_MUMBOS_HUT_1_ENTRANCE,    "Entrance" },
    { MAP_D_BGS_BUBBLEGLOOP_SWAMP, WARP_BGS_1_CROC_FRONT,             "Croc Front" },
    { MAP_D_BGS_BUBBLEGLOOP_SWAMP, WARP_BGS_2_WORLD_ENTRANCE,         "Warp Pad" },
    { MAP_D_BGS_BUBBLEGLOOP_SWAMP, WARP_BGS_3_TURTLE,                 "Tanktup Mouth" },
    { MAP_D_BGS_BUBBLEGLOOP_SWAMP, WARP_BGS_4_CROC_LEFT,              "Right Nostril" },
    { MAP_D_BGS_BUBBLEGLOOP_SWAMP, WARP_BGS_5_CROC_RIGHT,             "Left Nostril" },
    { MAP_D_BGS_BUBBLEGLOOP_SWAMP, WARP_BGS_6_MUMBOS_HUT,             "Outside Mumbo's Hut" },
    { MAP_D_BGS_BUBBLEGLOOP_SWAMP, WARP_BGS_14_WITCH_SWITCH,          "Switch Return" },
    { MAP_D_BGS_BUBBLEGLOOP_SWAMP, 91,                                "Demo?" },
    // LEVEL_5_FREEZEEZY_PEAK
    { MAP_27_FP_FREEZEEZY_PEAK, WARP_FP_1_WORLD_ENTRANCE,          "Warp Pad" },
    { MAP_27_FP_FREEZEEZY_PEAK, WARP_FP_6_WOZZA_CAVE,              "Outside Wozza's Cave" },
    { MAP_27_FP_FREEZEEZY_PEAK, WARP_FP_7_MUMBOS_HUT,              "Outside Mumbo's Hut" },
    { MAP_27_FP_FREEZEEZY_PEAK, WARP_FP_8_IGLOO,                   "Outside Igloo" },
    { MAP_27_FP_FREEZEEZY_PEAK, WARP_FP_9_TREE_BOTTOM,             "Christmas Tree" },
    { MAP_27_FP_FREEZEEZY_PEAK, WARP_FP_D_TREE_STAR,               "Lit Tree CS Return" },
    { MAP_27_FP_FREEZEEZY_PEAK, WARP_FP_15_WITCH_SWITCH,           "Switch Return" },
    { MAP_41_FP_BOGGYS_IGLOO,   WARP_FP_BOGGYS_IGLOO_1_ENTRANCE,   "Entrance" },
    { MAP_48_FP_MUMBOS_SKULL,   WARP_FP_MUMBOS_HUT_1_ENTRANCE,     "Entrance" },
    { MAP_53_FP_CHRISTMAS_TREE, WARP_FP_CHRISTMAS_TREE_1_ENTRANCE, "Inside" },
    { MAP_7F_FP_WOZZAS_CAVE,    WARP_FP_WOZZAS_CAVE_1_ENTRANCE,    "Entrance" },
    // LEVEL_6_LAIR
    { MAP_69_GL_MM_LOBBY,           WARP_GL_MM_LOBBY_1_NOTE_DOOR,                 "50 Note Door" },
    { MAP_69_GL_MM_LOBBY,           WARP_GL_MM_LOBBY_2_MM_ENTRANCE,               "Level Entrance" },
    { MAP_69_GL_MM_LOBBY,           WARP_GL_MM_LOBBY_A_PUZZLE,                    "Puzzle" },
    { MAP_69_GL_MM_LOBBY,           WARP_GL_MM_LOBBY_12_ENTRANCE,                 "Lair Entrance" },
    { MAP_6A_GL_TTC_AND_CC_PUZZLE,  WARP_GL_TTC_PUZZLE_1_LOWER_ENTRANCE,          "Lower" },
    { MAP_6A_GL_TTC_AND_CC_PUZZLE,  WARP_GL_TTC_PUZZLE_2_HIGHER_EXIT,             "Upper" },
    { MAP_6A_GL_TTC_AND_CC_PUZZLE,  WARP_GL_TTC_PUZZLE_8_CAULDRON,                "Purple Cauldron" },
    { MAP_6A_GL_TTC_AND_CC_PUZZLE,  WARP_GL_TTC_PUZZLE_A_CC_PUZZLE,               "CC Puzzle" },
    { MAP_6A_GL_TTC_AND_CC_PUZZLE,  WARP_GL_TTC_PUZZLE_B_TTC_PUZZLE,              "TTC Puzzle" },
    { MAP_6B_GL_180_NOTE_DOOR,      WARP_GL_CCW_PUZZLE_1_LOWER_ENTRANCE,          "Middle" },
    { MAP_6B_GL_180_NOTE_DOOR,      WARP_GL_CCW_PUZZLE_2_PIPE_ROOM,               "Big Pipe" },
    { MAP_6B_GL_180_NOTE_DOOR,      WARP_GL_CCW_PUZZLE_3_TO_TTC,                  "Red Eyes" },
    { MAP_6B_GL_180_NOTE_DOOR,      WARP_GL_CCW_PUZZLE_4_TO_CC,                   "Small Pipe" },
    { MAP_6B_GL_180_NOTE_DOOR,      WARP_GL_CCW_PUZZLE_5_NOTE_DOOR,             "Top" },
    { MAP_6B_GL_180_NOTE_DOOR,      WARP_GL_CCW_PUZZLE_A_WATERFALL,               "Waterfall" },
    { MAP_6B_GL_180_NOTE_DOOR,      WARP_GL_CCW_PUZZLE_B_PUZZLE,                  "Puzzle" },
    { MAP_6C_GL_RED_CAULDRON_ROOM,  WARP_GL_PIPE_ROOM_1_ENTRANCE,                 "Pipe" },
    { MAP_6C_GL_RED_CAULDRON_ROOM,  WARP_GL_PIPE_ROOM_8_CAULDRON,                 "Cauldron" },
    { MAP_6D_GL_TTC_LOBBY,          WARP_GL_TTC_LOBBY_1_ENTRANCE,                 "Stairs" },
    { MAP_6D_GL_TTC_LOBBY,          WARP_GL_TTC_LOBBY_4_TTC_ENTRANCE,             "Level Exit" },
    { MAP_6E_GL_GV_LOBBY,           WARP_GL_GV_LOBBY_1_LOWER_ENTRANCE,            "Stairs Down" },
    { MAP_6E_GL_GV_LOBBY,           WARP_GL_GV_LOBBY_2_HIGHER_EXIT,               "Higher Exit" },
    { MAP_6E_GL_GV_LOBBY,           WARP_GL_GV_LOBBY_3_GV_ENTRANCE,               "Level Entrance" },
    { MAP_6F_GL_FP_LOBBY,           WARP_GL_FP_LOBBY_1_LOWER_ENTRANCE,            "Lower" },
    { MAP_6F_GL_FP_LOBBY,           WARP_GL_FP_LOBBY_2_HIGHEST_EXIT,              "Upper" },
    { MAP_6F_GL_FP_LOBBY,           WARP_GL_FP_LOBBY_5_MOUTH_EXIT,                "Witch" },
    { MAP_6F_GL_FP_LOBBY,           WARP_GL_FP_LOBBY_6_FP_ENTRANCE,               "Level Entrance" },
    { MAP_6F_GL_FP_LOBBY,           WARP_GL_FP_LOBBY_8_LOWER_CAULDRON,            "Purple Cauldron" },
    { MAP_6F_GL_FP_LOBBY,           WARP_GL_FP_LOBBY_9_HIGHER_CAULDRON,           "Green Cauldron" },
    { MAP_70_GL_CC_LOBBY,           WARP_GL_CC_LOBBY_1_ENTRANCE,                  "Pipe" },
    { MAP_70_GL_CC_LOBBY,           WARP_GL_CC_LOBBY_2_CC_ENTRANCE,               "Level Entrance" },
    { MAP_70_GL_CC_LOBBY,           WARP_GL_CC_LOBBY_B_PUZZLE,                    "BGS Puzzle" },
    { MAP_71_GL_STATUE_ROOM,        WARP_GL_STATUE_ROOM_1_HIGHER_EXIT,            "Stairs Up" },
    { MAP_71_GL_STATUE_ROOM,        WARP_GL_STATUE_ROOM_2_LOWER_ENTRANCE,         "Down" },
    { MAP_71_GL_STATUE_ROOM,        WARP_GL_STATUE_ROOM_3_TO_BGS,                 "Wood Tunnel" },
    { MAP_71_GL_STATUE_ROOM,        WARP_GL_STATUE_ROOM_4_CEILING_EXIT,           "Ceiling" },
    { MAP_72_GL_BGS_LOBBY,          WARP_GL_BGS_LOBBY_1_ENTRANCE,                 "Entrance" },
    { MAP_72_GL_BGS_LOBBY,          WARP_GL_BGS_LOBBY_2_BGS_ENTRANCE,             "Level Entrance" },
    { MAP_72_GL_BGS_LOBBY,          WARP_GL_BGS_LOBBY_A_PUZZLE,                   "FP Puzzle" },
    { MAP_74_GL_GV_PUZZLE,          WARP_GL_GV_PUZZLE_1_TO_MMM,                   "Red Eyes" },
    { MAP_74_GL_GV_PUZZLE,          WARP_GL_GV_PUZZLE_2_ENTRANCE,                 "Lower" },
    { MAP_74_GL_GV_PUZZLE,          WARP_GL_GV_PUZZLE_A_PUZZLE,                   "Puzzle" },
    { MAP_75_GL_MMM_LOBBY,          WARP_GL_MMM_LOBBY_1_ENTRANCE,                 "Front" },
    { MAP_75_GL_MMM_LOBBY,          WARP_GL_MMM_LOBBY_2_MMM_ENTRANCE,              "Level Entrance" },
    { MAP_75_GL_MMM_LOBBY,          WARP_GL_MMM_LOBBY_3_COFFIN,                   "Crypt Door" },
    { MAP_76_GL_640_NOTE_DOOR,      WARP_GL_WATER_SWITCH_ROOM_1_LOWER_ENTRANCE,   "Mid" },
    { MAP_76_GL_640_NOTE_DOOR,      WARP_GL_WATER_SWITCH_ROOM_2_CCW_LOBBY_MUMBO_TOKEN, "Top Red" },
    { MAP_76_GL_640_NOTE_DOOR,      WARP_GL_WATER_SWITCH_ROOM_3_CCW_LOBBY,      "Top Green" },
    { MAP_76_GL_640_NOTE_DOOR,      WARP_GL_WATER_SWITCH_ROOM_4_UNDERWATER_EXIT,  "Red Eyes" },
    { MAP_77_GL_RBB_LOBBY,          WARP_GL_RBB_LOBBY_1_LOWER_ENTRANCE,           "Mid" },
    { MAP_77_GL_RBB_LOBBY,          WARP_GL_RBB_LOBBY_2_RBB_ENTRANCE,             "Level Entrance" },
    { MAP_77_GL_RBB_LOBBY,          WARP_GL_RBB_LOBBY_3_UNDERWATER_EXIT,          "Underwater Exit" },
    { MAP_77_GL_RBB_LOBBY,          WARP_GL_RBB_LOBBY_4_HIGHER_EXIT,              "Bricks" },
    { MAP_77_GL_RBB_LOBBY,          WARP_GL_RBB_LOBBY_8_CAULDRON,                 "Green Cauldron" },
    { MAP_77_GL_RBB_LOBBY,          WARP_GL_RBB_LOBBY_A_SWITCH,                   "Switch CS Return" },
    { MAP_78_GL_RBB_AND_MMM_PUZZLE, WARP_GL_MMM_PUZZLE_1_UNDERWATER_ENTRANCE,     "MMM Entrance" },
    { MAP_78_GL_RBB_AND_MMM_PUZZLE, WARP_GL_MMM_PUZZLE_2_HIGHER_ENTRANCE,         "RBB Entrance" },
    { MAP_78_GL_RBB_AND_MMM_PUZZLE, WARP_GL_MMM_PUZZLE_5_RBB_PUZZLE,              "RBB Puzzle" },
    { MAP_78_GL_RBB_AND_MMM_PUZZLE, WARP_GL_MMM_PUZZLE_6_MMM_PUZZLE,              "MMM Puzzle" },
    { MAP_79_GL_CCW_LOBBY,          WARP_GL_CCW_LOBBY_1_WHIPCRACK_ENTRANCE,       "Whipcracks" },
    { MAP_79_GL_CCW_LOBBY,          WARP_GL_CCW_LOBBY_2_HIGHER_ENTRANCE,          "Red Dirt" },
    { MAP_79_GL_CCW_LOBBY,          WARP_GL_CCW_LOBBY_3_FURNACE_EXIT,             "Green" },
    { MAP_79_GL_CCW_LOBBY,          WARP_GL_CCW_LOBBY_6_CCW_ENTRANCE,             "Level Entrance" },
    { MAP_79_GL_CCW_LOBBY,          WARP_GL_CCW_LOBBY_8_CAULDRON,                 "Red Cauldron" },
    { MAP_79_GL_CCW_LOBBY,          WARP_GL_CCW_LOBBY_12_SWITCH,                  "Switch CS Return" },
    { MAP_7A_GL_CRYPT,              WARP_GL_COFFIN_ROOM_1_ENTRANCE,               "Entrance" },
    { MAP_7A_GL_CRYPT,              WARP_GL_COFFIN_ROOM_A_SWITCH,                 "Switch" },
    { MAP_80_GL_FF_ENTRANCE,        WARP_GL_FURNACE_FUN_PATH_1_ENTRANCE,          "Lower" },
    { MAP_80_GL_FF_ENTRANCE,        WARP_GL_FURNACE_FUN_PATH_2_EXIT,              "Pad" },
    { MAP_8E_GL_FURNACE_FUN,        WARP_GL_FURNACE_FUN_1_AFTER_PAD,              "After Pad" },
    { MAP_8E_GL_FURNACE_FUN,        WARP_GL_FURNACE_FUN_2_ENTRANCE_PAD,           "Pad" },
    { MAP_8E_GL_FURNACE_FUN,        WARP_GL_FURNACE_FUN_4_AFTER_BOARD,            "After Board" },
    { MAP_8E_GL_FURNACE_FUN,        WARP_GL_FURNACE_FUN_5_TO_TOWER,               "Back Exit" },
    { MAP_8E_GL_FURNACE_FUN,        5,                                            "GL Furnace Fun - 5" },
    { MAP_8E_GL_FURNACE_FUN,        WARP_GL_FURNACE_FUN_8_CAULDRON,               "FF Yellow Cauldron" },
    { MAP_93_GL_DINGPOT,            WARP_GL_TOWER_2_DINGPOT,                      "Dingpot" },
    { MAP_93_GL_DINGPOT,            WARP_GL_TOWER_5_ENTRANCE,                     "Lower" },
    { MAP_93_GL_DINGPOT,            WARP_GL_TOWER_8_CAULDRON,                     "Yellow Cauldron" },
    { MAP_93_GL_DINGPOT,            WARP_GL_TOWER_A_GRUNTY_PUZZLE,                "Puzzle" },
    // LEVEL_7_GOBIS_VALLEY
    { MAP_12_GV_GOBIS_VALLEY,    WARP_GV_2_JINXY_SPHINX,                  "Jinxy Door" },
    { MAP_12_GV_GOBIS_VALLEY,    WARP_GV_3_MEMORY_PYRAMID,                "Sun Door" },
    { MAP_12_GV_GOBIS_VALLEY,    WARP_GV_4_MAZE_PYRAMID_FRONT,            "Central Pyramid Entrance" },
    { MAP_12_GV_GOBIS_VALLEY,    WARP_GV_5_WATER_PYRAMID,                 "Lower Star Door" },
    { MAP_12_GV_GOBIS_VALLEY,    WARP_GV_6_RUBEE_PYRAMID,                 "Kazooie Door" },
    { MAP_12_GV_GOBIS_VALLEY,    WARP_GV_7_MAZE_PYRAMID_BACK,             "Central Pyramid Exit" },
    { MAP_12_GV_GOBIS_VALLEY,    WARP_GV_8_WORLD_ENTRANCE,                "Warp Pad" },
    { MAP_12_GV_GOBIS_VALLEY,    WARP_GV_A_EGG_ROOM,                      "Outside SNS Tomb" },
    { MAP_13_GV_MEMORY_GAME,     WARP_GV_MEMORY_PYRAMID_1_ENTRANCE,       "Entrance" },
    { MAP_13_GV_MEMORY_GAME,     WARP_GV_MEMORY_PYRAMID_2_CENTER,         "Center" },
    { MAP_14_GV_SANDYBUTTS_MAZE, WARP_GV_MAZE_PYRAMID_1_ENTRANCE,         "Entrance" },
    { MAP_15_GV_WATER_PYRAMID,   WARP_GV_WATER_PYRAMID_1_RETURN_TO_JIGGY, "Return To Jiggy" },
    { MAP_15_GV_WATER_PYRAMID,   WARP_GV_WATER_PYRAMID_2_TOP,         "Ceiling" },
    { MAP_15_GV_WATER_PYRAMID,   WARP_GV_WATER_PYRAMID_6_LOWER_EXIT,      "Lower Exit" },
    { MAP_16_GV_RUBEES_CHAMBER,  WARP_GV_RUBEE_PYRAMID_7_ENTRANCE,       "Entrance" },
    { MAP_1A_GV_INSIDE_JINXY,    WARP_GV_INSIDE_JINXY_2_ENTRANCE,         "Entrance" },
    { MAP_92_GV_SNS_CHAMBER,     WARP_GV_EGG_ROOM_5_ENTRANCE,             "Entrance" },
    // LEVEL_8_CLICK_CLOCK_WOOD
    { MAP_40_CCW_HUB,                         WARP_CCW_1_WINTER,                     "Winter" },
    { MAP_40_CCW_HUB,                         WARP_CCW_2_SPRING,                     "Spring" },
    { MAP_40_CCW_HUB,                         WARP_CCW_3_SUMMER,                     "Summer" },
    { MAP_40_CCW_HUB,                         WARP_CCW_4_AUTUMN,                     "Autumn" },
    { MAP_40_CCW_HUB,                         WARP_CCW_5_SPRING_SWITCH,              "Spring Switch" },
    { MAP_40_CCW_HUB,                         WARP_CCW_7_WORLD_ENTRANCE,             "Warp Pad" },
    { MAP_43_CCW_SPRING,                      WARP_CCW_SPRING_1_ENTRANCE,            "Entrance" },
    { MAP_43_CCW_SPRING,                      WARP_CCW_SPRING_4_NABNUT_WINDOW,       "Nabnut's Window" },
    { MAP_43_CCW_SPRING,                      WARP_CCW_SPRING_5_SUMMER_SWITCH,       "Summer Switch" },
    { MAP_43_CCW_SPRING,                      WARP_CCW_SPRING_6_ZUBBA_HIVE_FRONT,          "Outside Zubba Hive" },
    { MAP_43_CCW_SPRING,                      WARP_CCW_SPRING_7_NABNUT_DOOR,        "Outside Nabnut's House" },
    { MAP_43_CCW_SPRING,                      WARP_CCW_SPRING_8_WHIPCRACK,            "Top Door" },
    { MAP_43_CCW_SPRING,                      WARP_CCW_SPRING_9_MUMBOS_HUT,          "Outside Mumbo's Hut" },
    { MAP_44_CCW_SUMMER,                      WARP_CCW_SUMMER_1_ENTRANCE,            "Entrance" },
    { MAP_44_CCW_SUMMER,                      WARP_CCW_SUMMER_4_NABNUT_WINDOW,       "Outside Nabnut's Window" },
    { MAP_44_CCW_SUMMER,                      WARP_CCW_SUMMER_5_AUTUMN_SWITCH,       "Autumn Switch" },
    { MAP_44_CCW_SUMMER,                      WARP_CCW_SUMMER_6_ZUBBA_HIVE_TOP,          "Outside Zubba Hive" },
    { MAP_44_CCW_SUMMER,                      WARP_CCW_SUMMER_7_NABNUT_DOOR,        "Outside Nabnut's House" },
    { MAP_44_CCW_SUMMER,                      WARP_CCW_SUMMER_8_WHIPCRACK,            "Top Door" },
    { MAP_44_CCW_SUMMER,                      WARP_CCW_SUMMER_9_MUMBOS_HUT,          "Outside Mumbo's Hut" },
    { MAP_45_CCW_AUTUMN,                      WARP_CCW_AUTUMN_1_ENTRANCE,            "Entrance" },
    { MAP_45_CCW_AUTUMN,                      WARP_CCW_AUTUMN_4_NABNUT_WINDOW,       "Outside Nabnut's Window" },
    { MAP_45_CCW_AUTUMN,                      WARP_CCW_AUTUMN_5_ZUBBA_HIVE_TOP,          "Outside Zubba Hive" },
    { MAP_45_CCW_AUTUMN,                      WARP_CCW_AUTUMN_6_ATTIC, "Outside Nabnut's Upper Window" },
    { MAP_45_CCW_AUTUMN,                      WARP_CCW_AUTUMN_7_NABNUT_DOOR,        "Outside Nabnut's House" },
    { MAP_45_CCW_AUTUMN,                      WARP_CCW_AUTUMN_8_WHIPCRACK,            "Top Door" },
    { MAP_45_CCW_AUTUMN,                      WARP_CCW_AUTUMN_9_MUMBOS_HUT,          "Outside Mumbo's Hut" },
    { MAP_45_CCW_AUTUMN,                      WARP_CCW_AUTUMN_F_WINTER_SWITCH,       "Winter Switch" },
    { MAP_46_CCW_WINTER,                      WARP_CCW_WINTER_1_ENTRANCE,            "Entrance" },
    { MAP_46_CCW_WINTER,                      WARP_CCW_WINTER_4_NABNUT_WINDOW,       "Outside Nabnut's Window" },
    { MAP_46_CCW_WINTER,                      WARP_CCW_WINTER_5_ATTIC,      "Outside Highest Window" },
    { MAP_46_CCW_WINTER,                      WARP_CCW_WINTER_6_NABNUT_WINDOW_UPPER, "Outside Nabnut's Upper Window" },
    { MAP_46_CCW_WINTER,                      WARP_CCW_WINTER_8_WHIPCRACK,            "Top Door" },
    { MAP_46_CCW_WINTER,                      WARP_CCW_WINTER_9_MUMBOS_HUT,          "Outside Mumbo's Hut" },
    { MAP_46_CCW_WINTER,                      WARP_CCW_WINTER_15_WITCH_SWITCH,       "Witch Switch" },
    { MAP_46_CCW_WINTER,                      91,                                    "Demo" },
    { MAP_4A_CCW_SPRING_MUMBOS_SKULL,         WARP_CCW_MUMBOS_HUT_SPRING_1_ENTRANCE, "Entrance" },
    { MAP_4B_CCW_SUMMER_MUMBOS_SKULL,         WARP_CCW_MUMBOS_HUT_SUMMER_1_ENTRANCE, "Entrance" },
    { MAP_4C_CCW_AUTUMN_MUMBOS_SKULL,         WARP_CCW_MUMBOS_HUT_AUTUMN_1_ENTRANCE, "Entrance" },
    { MAP_4D_CCW_WINTER_MUMBOS_SKULL,         WARP_CCW_MUMBOS_HUT_WINTER_1_ENTRANCE, "Entrance" },
    { MAP_5A_CCW_SUMMER_ZUBBA_HIVE,           WARP_CCW_BEEHIVE_SUMMER_2_TOP,         "Top" },
    { MAP_5A_CCW_SUMMER_ZUBBA_HIVE,           WARP_CCW_BEEHIVE_SUMMER_3_REDO,        "Redo" },
    { MAP_5B_CCW_SPRING_ZUBBA_HIVE,           WARP_CCW_BEEHIVE_SPRING_1_ENTRANCE,    "Entrance" },
    { MAP_5C_CCW_AUTUMN_ZUBBA_HIVE,           WARP_CCW_BEEHIVE_AUTUMN_2_TOP,    "Entrance" },
    { MAP_5E_CCW_SPRING_NABNUTS_HOUSE,        WARP_CCW_NABNUT_SPRING_1_DOOR,         "Door" },
    { MAP_5E_CCW_SPRING_NABNUTS_HOUSE,        WARP_CCW_NABNUT_SPRING_2_WINDOW,       "Window" },
    { MAP_5F_CCW_SUMMER_NABNUTS_HOUSE,        WARP_CCW_NABNUT_SUMMER_1_DOOR,         "Door" },
    { MAP_5F_CCW_SUMMER_NABNUTS_HOUSE,        WARP_CCW_NABNUT_SUMMER_2_WINDOW,       "Window" },
    { MAP_60_CCW_AUTUMN_NABNUTS_HOUSE,        WARP_CCW_NABNUT_AUTUMN_1_DOOR,         "Door" },
    { MAP_60_CCW_AUTUMN_NABNUTS_HOUSE,        WARP_CCW_NABNUT_AUTUMN_2_WINDOW,       "Window" },
    { MAP_61_CCW_WINTER_NABNUTS_HOUSE,        2,                                     "Window" },
    { MAP_62_CCW_WINTER_HONEYCOMB_ROOM,       WARP_CCW_ATTIC_WINTER_1_ENTRANCE,      "Entrance" },
    { MAP_63_CCW_AUTUMN_NABNUTS_WATER_SUPPLY, WARP_CCW_WATER_ROOM_AUTUMN_1_ENTRANCE, "Entrance" },
    { MAP_64_CCW_WINTER_NABNUTS_WATER_SUPPLY, WARP_CCW_WATER_ROOM_WINTER_1_ENTRANCE, "Entrance" },
    { MAP_65_CCW_SPRING_WHIPCRACK_ROOM,       WARP_CCW_WHIPCRACK_SPRING_1_ENTRANCE,  "Entrance" },
    { MAP_66_CCW_SUMMER_WHIPCRACK_ROOM,       WARP_CCW_WHIPCRACK_SUMMER_1_ENTRANCE,  "Entrance" },
    { MAP_67_CCW_AUTUMN_WHIPCRACK_ROOM,       WARP_CCW_WHIPCRACK_AUTUMN_1_ENTRANCE,  "Entrance" },
    { MAP_68_CCW_WINTER_WHIPCRACK_ROOM,       WARP_CCW_WHIPCRACK_WINTER_1_ENTRANCE,  "Entrance" },
    // LEVEL_9_RUSTY_BUCKET_BAY
    { MAP_31_RBB_RUSTY_BUCKET_BAY, WARP_RBB_1_CAPTAIN_ROOM_WINDOW,    "Captain Room Window" },
    { MAP_31_RBB_RUSTY_BUCKET_BAY, WARP_RBB_2_CREW_ROOM_WINDOW,       "Crew Room Window" },
    { MAP_31_RBB_RUSTY_BUCKET_BAY, WARP_RBB_3_ENGINE_SWITCH_PIPE,     "Engine Switch Pipe" },
    { MAP_31_RBB_RUSTY_BUCKET_BAY, WARP_RBB_4_KITCHEN_PIPE,           "Kitchen Pipe" },
    { MAP_31_RBB_RUSTY_BUCKET_BAY, WARP_RBB_5_NAV_ROOM_PIPE,          "Nav Room Pipe" },
    { MAP_31_RBB_RUSTY_BUCKET_BAY, WARP_RBB_6_STORAGE_PIPE,           "Storage Pipe" },
    { MAP_31_RBB_RUSTY_BUCKET_BAY, WARP_RBB_7_ENGINE_ENTRANCE,        "Engine Entrance" },
    { MAP_31_RBB_RUSTY_BUCKET_BAY, WARP_RBB_8_BOAT_ROOM,              "Boat Room" },
    { MAP_31_RBB_RUSTY_BUCKET_BAY, WARP_RBB_9_FIRST_CONTAINER,        "First Container" },
    { MAP_31_RBB_RUSTY_BUCKET_BAY, WARP_RBB_A_SECOND_CONTAINER,       "Second Container" },
    { MAP_31_RBB_RUSTY_BUCKET_BAY, WARP_RBB_B_THIRD_CONTAINER,        "Third Container" },
    { MAP_31_RBB_RUSTY_BUCKET_BAY, WARP_RBB_C_AFTER_BOSS,             "After Boss" },
    { MAP_31_RBB_RUSTY_BUCKET_BAY, WARP_RBB_D_FISH_WAREHOUSE,         "Fish Warehouse" },
    { MAP_31_RBB_RUSTY_BUCKET_BAY, WARP_RBB_E_WITCH_SWITCH,           "Witch Switch" },
    { MAP_31_RBB_RUSTY_BUCKET_BAY, WARP_RBB_10_WORLD_ENTRANCE,        "World Entrance" },
    { MAP_31_RBB_RUSTY_BUCKET_BAY, WARP_RBB_13_ANCHOR_ROOM,           "Anchor Room" },
    { MAP_34_RBB_ENGINE_ROOM,      WARP_RBB_ENGINE_1_ENTRANCE,        "Entrance" },
    { MAP_34_RBB_ENGINE_ROOM,      WARP_RBB_ENGINE_2_LEFT_SWITCH,     "Left Switch" },
    { MAP_34_RBB_ENGINE_ROOM,      WARP_RBB_ENGINE_3_RIGHT_SWITCH,    "Right Switch" },
    { MAP_34_RBB_ENGINE_ROOM,      WARP_RBB_ENGINE_4_PIPE_ENTRANCE,   "Pipe Entrance" },
    { MAP_35_RBB_WAREHOUSE,        WARP_RBB_WAREHOUSE_1_DOOR,         "Door" },
    { MAP_35_RBB_WAREHOUSE,        WARP_RBB_WAREHOUSE_2_WINDOW,       "Window" },
    { MAP_36_RBB_BOATHOUSE,        WARP_RBB_BOAT_1_ENTRANCE,          "Entrance" },
    { MAP_37_RBB_CONTAINER_1,      WARP_RBB_CONTAINER_1_ENTRANCE,     "Entrance" },
    { MAP_38_RBB_CONTAINER_3,      WARP_RBB_CONTAINER_3_ENTRANCE,     "Entrance" },
    { MAP_39_RBB_CREW_CABIN,       WARP_RBB_CREW_CABIN_1_ENTRANCE,    "Entrance" },
    { MAP_3A_RBB_BOSS_BOOM_BOX,    WARP_RBB_BOSS_1_ENTRANCE,          "Entrance" },
    { MAP_3A_RBB_BOSS_BOOM_BOX,    WARP_RBB_BOSS_2_REDO,              "Redo" },
    { MAP_3B_RBB_STORAGE_ROOM,     WARP_RBB_STORAGE_1_ENTRANCE,       "Entrance" },
    { MAP_3C_RBB_KITCHEN,          WARP_RBB_KITCHEN_1_ENTRANCE,       "Entrance" },
    { MAP_3D_RBB_NAVIGATION_ROOM,  WARP_RBB_NAVIGATION_1_ENTRANCE,    "Entrance" },
    { MAP_3E_RBB_CONTAINER_2,      WARP_RBB_CONTAINER_2_ENTRANCE,     "Entrance" },
    { MAP_3F_RBB_CAPTAINS_CABIN,   WARP_RBB_CAPTAIN_CABIN_1_ENTRANCE, "Entrance" },
    { MAP_8B_RBB_ANCHOR_ROOM,      WARP_RBB_ANCHOR_2_ENTRANCE,        "Entrance" },
    { MAP_8B_RBB_ANCHOR_ROOM,      WARP_RBB_ANCHOR_4_SWITCH,          "Switch" },
    // LEVEL_A_MAD_MONSTER_MANSION
    { MAP_1B_MMM_MAD_MONSTER_MANSION, WARP_MMM_1_MANSION_DOOR,                "Mansion Door" },
    { MAP_1B_MMM_MAD_MONSTER_MANSION, WARP_MMM_3_WELL_TOP,                    "Well Top" },
    { MAP_1B_MMM_MAD_MONSTER_MANSION, WARP_MMM_4_SHED,                        "Shed" },
    { MAP_1B_MMM_MAD_MONSTER_MANSION, WARP_MMM_5_CHURCH_DOOR,                 "Church Door" },
    { MAP_1B_MMM_MAD_MONSTER_MANSION, WARP_MMM_6_CHURCH_WINDOW,               "Church Window" },
    { MAP_1B_MMM_MAD_MONSTER_MANSION, WARP_MMM_7_CHIMNEY,                     "Chimney" },
    { MAP_1B_MMM_MAD_MONSTER_MANSION, WARP_MMM_8_DRAINPIPE_BOTTOM,            "Outside Drainpipe" },
    { MAP_1B_MMM_MAD_MONSTER_MANSION, WARP_MMM_9_CELLAR,                      "Outside Cellar" },
    { MAP_1B_MMM_MAD_MONSTER_MANSION, WARP_MMM_A_F1_RED_FEATHER_WINDOW,       "F1 Red Feather Window" },
    { MAP_1B_MMM_MAD_MONSTER_MANSION, WARP_MMM_B_F1_EGG_WINDOW,               "F1 Egg Window" },
    { MAP_1B_MMM_MAD_MONSTER_MANSION, WARP_MMM_C_F2_BATHROOM_WINDOW,          "F2 Bathroom Window" },
    { MAP_1B_MMM_MAD_MONSTER_MANSION, WARP_MMM_D_F2_GOLD_FEATHER_WINDOW,      "F2 Gold Feather Window" },
    { MAP_1B_MMM_MAD_MONSTER_MANSION, WARP_MMM_E_F3_BEDROOM_WINDOW,           "F3 Bedroom Window" },
    { MAP_1B_MMM_MAD_MONSTER_MANSION, WARP_MMM_F_F3_NOTE_WINDOW,              "F3 Note Window" },
    { MAP_1B_MMM_MAD_MONSTER_MANSION, WARP_MMM_10_CHURCH_TOP,                 "Church Top" },
    { MAP_1B_MMM_MAD_MONSTER_MANSION, WARP_MMM_11_CHURCH_CLOCK,               "Church Clock" },
    { MAP_1B_MMM_MAD_MONSTER_MANSION, WARP_MMM_12_MUMBOS_HUT,                 "Outside Mumbo's Hut" },
    { MAP_1B_MMM_MAD_MONSTER_MANSION, WARP_MMM_13_WELL_BOTTOM,                "Well Bottom" },
    { MAP_1B_MMM_MAD_MONSTER_MANSION, WARP_MMM_14_WORLD_ENTRANCE,             "World Entrance" },
    { MAP_1C_MMM_CHURCH,              WARP_MMM_CHURCH_1_ENTRANCE,             "Entrance" },
    { MAP_1C_MMM_CHURCH,              WARP_MMM_CHURCH_14_WITCH_SWITCH,        "Witch Switch" },
    { MAP_1D_MMM_CELLAR,              WARP_MMM_CELLAR_1_ENTRANCE,             "Entrance" },
    { MAP_24_MMM_TUMBLARS_SHED,       WARP_MMM_SHED_1_ENTRANCE,               "Entrance" },
    { MAP_25_MMM_WELL,                WARP_MMM_WELL_1_TOP_ENTRANCE,           "Top" },
    { MAP_25_MMM_WELL,                WARP_MMM_WELL_4_BOTTOM_ENTRANCE,        "Bottom" },
    { MAP_26_MMM_NAPPERS_ROOM,        WARP_MMM_DINING_1_FRONT_ENTRANCE,       "Front" },
    { MAP_26_MMM_NAPPERS_ROOM,        WARP_MMM_DINING_2_CHIMNEY,              "Chimney" },
    { MAP_28_MMM_EGG_ROOM,            WARP_MMM_EGG_1_ENTRANCE,           "Entrance" },
    { MAP_29_MMM_NOTE_ROOM,           WARP_MMM_NOTE_1_ENTRANCE,          "Entrance" },
    { MAP_2A_MMM_FEATHER_ROOM,        WARP_MMM_RED_FEATHER_1_ENTRANCE,        "Entrance" },
    { MAP_2B_MMM_SECRET_CHURCH_ROOM,  WARP_MMM_CHURCH_WINDOW_1_ENTRANCE, "Entrance" },
    { MAP_2C_MMM_BATHROOM,            WARP_MMM_BATHROOM_1_ENTRANCE,           "Entrance" },
    { MAP_2C_MMM_BATHROOM,            WARP_MMM_BATHROOM_4_LOGGO,              "Loggo" },
    { MAP_2D_MMM_BEDROOM,             WARP_MMM_BEDROOM_1_ENTRANCE,            "Entrance" },
    { MAP_2E_MMM_HONEYCOMB_ROOM,      WARP_MMM_HONEYCOMB_ROOM_1_ENTRANCE,     "Entrance" },
    { MAP_2F_MMM_WATERDRAIN_BARREL,   WARP_MMM_DRAINPIPE_1_TOP_ENTRANCE,      "Top" },
    { MAP_2F_MMM_WATERDRAIN_BARREL,   WARP_MMM_DRAINPIPE_2_BOTTOM_EXIT,       "Bottom" },
    { MAP_30_MMM_MUMBOS_SKULL,        WARP_MMM_MUMBOS_HUT_1_ENTRANCE,         "Entrance" },
    { MAP_8D_MMM_INSIDE_LOGGO,        WARP_MMM_SEPTIC_TANK_4_ENTRANCE,        "Entrance" },
    // LEVEL_B_SPIRAL_MOUNTAIN
    { MAP_1_SM_SPIRAL_MOUNTAIN, WARP_SM_12_FROM_HOUSE,    "Outside Banjo's House" },
    { MAP_1_SM_SPIRAL_MOUNTAIN, WARP_SM_13_LAIR_ENTRANCE, "Lair Entrance" },
    { MAP_8C_SM_BANJOS_HOUSE,   WARP_SM_BANJOS_HOUSE_1_ENTRANCE,          "Banjo's House" },
    { MAP_8C_SM_BANJOS_HOUSE,   WARP_SM_BANJOS_HOUSE_2_BOTTLES,           "SM Banjo's House - Bottles" },
    // LEVEL_C_BOSS
    { MAP_90_GL_BATTLEMENTS, WARP_GL_BOSS_1_ENTRANCE, "GL Battlements - Entrance" },
    // LEVEL_D_CUTSCENE (not in Warps.cpp): the 100% ending, as the game's own goes there after Grunty (cutscene_skip.c),
    // so Mumbo's pictures can be tested without the fight
    { MAP_95_CS_END_ALL_100, 1, "100% ending" },
};
// clang-format on

#define WARP_ENTRIES LEN(kWarps)
#define CVAR_WARP_AREA "gLusx.WarpArea"         // the last warp's row and entrance there, for the next time
#define CVAR_WARP_ENTRANCE "gLusx.WarpEntrance"

// an area: one of the game's maps, its entrances the entries after its first in kWarps
static struct {
    int first, count;
} sAreas[WARP_ENTRIES];
static int sAreaCount;
static Option sWarpRows[WARP_ENTRIES];
static u8 sWarpEntrance[WARP_ENTRIES];
static int sWarpTo = -1; // the area picked, for when the menu has closed

// the area's entrance number `n`: its entries are where they are in kWarps, not always one after another
static const WarpEntry& AreaEntry(int area, int n) {
    const int map = kWarps[sAreas[area].first].map;

    for (int i = sAreas[area].first; i < WARP_ENTRIES; i++) {
        if (kWarps[i].map == map && n-- == 0) {
            return kWarps[i];
        }
    }
    return kWarps[sAreas[area].first];
}

static void StepWarpEntrance(int area, int direction) {
    sWarpEntrance[area] = Wrap(sWarpEntrance[area] + direction, 0, sAreas[area].count - 1);
}

static const char* TextWarpEntrance(int area, char*, size_t) {
    return AreaEntry(area, sWarpEntrance[area]).name;
}

static void PressWarp(int area) {
    sWarpTo = area;
    CVarSetInteger(CVAR_WARP_AREA, area);
    CVarSetInteger(CVAR_WARP_ENTRANCE, sWarpEntrance[area]);
}

void LusxWarp_Open(const Option** list, int* count, int* cursor, int* pinned) {
    if (sAreaCount == 0) {
        // the areas, in the list's order: each map where it first comes, with every entrance of it the list has
        for (int i = 0; i < WARP_ENTRIES; i++) {
            bool seen = false;

            for (int j = 0; j < i && !seen; j++) {
                seen = kWarps[j].map == kWarps[i].map;
            }
            if (!seen) {
                sAreas[sAreaCount].first = i;
                sAreas[sAreaCount].count = 0;
                for (int j = i; j < WARP_ENTRIES; j++) {
                    sAreas[sAreaCount].count += kWarps[j].map == kWarps[i].map;
                }
                sAreaCount++;
            }
        }
        for (int i = 0; i < sAreaCount; i++) {
            const int map = kWarps[sAreas[i].first].map;
            Option& row = sWarpRows[i];

            row.kind = OPT_CUSTOM;
            row.label = map >= 0 && map < MAP_NUM_MAPS ? mapNames[map].displayName : "?";
            row.arg = i;
            row.step = StepWarpEntrance;
            row.text = TextWarpEntrance;
            row.press = PressWarp;
        }
    }
    const int last = Clamp(CVarGetInteger(CVAR_WARP_AREA, 0), 0, sAreaCount - 1);

    sWarpEntrance[last] = Clamp(CVarGetInteger(CVAR_WARP_ENTRANCE, 0), 0, sAreas[last].count - 1);
    *list = sWarpRows;
    *count = sAreaCount;
    *cursor = last;
    *pinned = 0;
}

void LusxWarp_Run(void) {
    if (sWarpTo >= 0) {
        const WarpEntry& to = AreaEntry(sWarpTo, sWarpEntrance[sWarpTo]);
        const int map = to.map, exit = to.exit;

        Lighthouse::RunOnGameThread([map, exit] { func_8031D04C((enum map_e)map, exit); });
        sWarpTo = -1;
    }
}
