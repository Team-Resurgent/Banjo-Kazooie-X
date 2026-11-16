// lhx_torch.cpp -- Banjo-Kazooie-X: the rest of what Lighthouse's game side takes from Torch.
//
// Romhack configs (an archive's assets/aGameConfig, src/port/Romhack/RomhackConfig.cpp): Torch makes and reads them on
// a PC, for a romhack's ROM; the Xbox build is the game as it shipped, and an archive with one is turned down here, so
// Lighthouse plays it with the game's own tables, as it does an archive without one.
#include "factories/bk64/ConfigFactory.h"

namespace BK64 {

bool ReadGameConfig(const std::vector<char>& blob, GameConfigData& out, std::string& error) {
    (void)blob;
    (void)out;
    error = "romhack configs are not read on the Xbox";
    return false;
}

} // namespace BK64
