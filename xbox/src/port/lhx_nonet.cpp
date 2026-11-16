// lhx_nonet.cpp -- Banjo-Kazooie-X: Lighthouse's online play (Anchor, src/port/Network/) is left out: the Xbox build
// has no SDL2_net, and plays alone. What the rest of the game asks of it:
//   - world sync is never on
//   - the per-level stores Anchor replicates (breakables, huts, egg tolls, puzzle steps) are Anchor's own
//     (Network/Anchor/Packets/): with no Anchor there are none, so a level's reset has nothing of them to clear
#include <stdint.h>

extern "C" int32_t port_anchor_isWorldSyncActive(void) {
    return 0;
}

void port_breakable_clearForLevel(int32_t levelId) {
    (void)levelId;
}

void port_hutSmash_clearForLevel(int32_t levelId) {
    (void)levelId;
}

void port_eggToll_clearForLevel(int32_t levelId) {
    (void)levelId;
}

void port_puzzleStep_clearForLevel(int32_t levelId) {
    (void)levelId;
}
