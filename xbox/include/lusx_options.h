// lusx_options.h -- the rows of the port's menu (xbox/src/lusx_menu.cpp).
#pragma once

#include <stddef.h>

enum OptionKind {
    OPT_ON_OFF,  // an integer setting, 0 or 1
    OPT_LIST,    // an integer setting: one of `names`, its value `values[i]`, or i with no `values`
    OPT_PERCENT, // an integer setting, 0 to 100 in tens (the volumes); `fallback` is its default in tenths
    OPT_CUSTOM,  // no setting: `step` changes something, by a press of left (-1) or right (1), `text` shows it
    OPT_ACTION,  // the same, but something done, by A
};

struct Option {
    OptionKind kind;
    const char* label;
    const char* cvar;
    int fallback; // the setting's value when it was never set: the game's default for it
    const char* const* names;
    const int* values;
    int count;
    int seqPlayer;       // a volume: the audio engine's player it scales, or -1
    void (*after)(void); // what else the change needs
    int arg;             // OPT_CUSTOM's: what `step` and `text` are told
    void (*step)(int arg, int direction);
    const char* (*text)(int arg, char* buffer, size_t size);
    void (*press)(int arg); // A does this and closes the menu
};

// a page of lists: a row for each
struct OptionGroup {
    const char* title;
    const Option* list;
    int count;
};

// the Debug page's Edit save, for when a save is being played (xbox/src/port/lhx_save_edit.cpp: in the game's group, as
// it needs its headers). Not const: its per-world pages' lengths are counted at start-up.
extern OptionGroup kEditGroups[];
extern const int kEditGroupCount;
bool LusxEdit_Available(void);

// the Debug page's Warp: its rows, how many, the row to start on (the last warp) and how many stay put while the
// areas scroll; the warp itself, once the menu has closed
void LusxWarp_Open(const Option** list, int* count, int* cursor, int* pinned);
void LusxWarp_Run(void);

#ifdef __cplusplus
#include <string>
#include <vector>

// the Mods page: the archives in mods/ beside default.xbe, as this run's start found them (xbox/src/port/lhx_nomods.cpp,
// after Mario-Kart-64XX's), each on or off, in their order: the top one wins where two change the same thing. The ones
// turned on (each is off until it is) and the order are kept in the config, and read only at the start
struct LusxMod {
    std::string file; // its name in the mods folder
    bool on;          // as the config had it at the start: loaded, or not
    int start;        // its place at the start
};
const std::vector<LusxMod>& LusxMods_List(void); // in the order as set now
bool LusxMods_IsOff(const std::string& file);    // in the config, now
void LusxMods_SetOff(const std::string& file, bool off);
void LusxMods_Move(int index, int by); // up (by < 0) or down the list, and the config
#endif
