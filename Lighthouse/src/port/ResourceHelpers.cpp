
#include "ship/resource/ResourceManager.h"
#include "ship/resource/archive/Archive.h"
#include "ship/resource/archive/ArchiveManager.h"
#include "ship/resource/File.h"
#include "ship/resource/type/Blob.h"
#include "fast/resource/ResourceType.h"
#include "fast/resource/type/DisplayList.h"
#include "libultraship/bridge/resourcebridge.h"
#include "libultraship/libultraship.h"
#include "ship/Context.h"
#include "Resource/Importers/MusicFactory.h"
#include "UI/cvar_prefixes.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cstring>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#if defined(_DEBUG) && defined(_MSC_VER)
#include <crtdbg.h>
#endif

#include "Resource/AssetTrace.h"

#include "GameVersion/BaseGameVersion.h"
#include "Localization/Language.h"

#include "enums.h"

extern "C" void func_8031B5C4(int32_t lang); // decomp: set dialog language index

extern "C" {
// SNS picture demo table (core2/map/exit.c, Struct_core2_C4320_0). unk2 is both
// the demo file slot and the map exit/entry-point id used for the warp.
typedef struct {
    uint8_t map, unk1, exit, unk3, unk4, unk5;
} BKSnsDemoEntry;
extern BKSnsDemoEntry D_80371F78[3];
}

// Dialog language state — detected at boot from o2r version
static int sDialogLanguageCount = 1;  // 1 for US/JP, 3 for PAL (EN/FR/DE)
static int sDialogLanguage = 0;       // 0=English, 1=French, 2=German
static int sScopedDialogLanguage = 0; // the same, within a hack's own language pack
static bool sIsJapanese = false;      // true when a JP o2r is loaded

namespace {
std::unordered_set<uint32_t> sEmptyAssetSlots;

const std::unordered_map<uint32_t, std::string>& GetAssetSymbolMap() {
    static std::once_flag mapOnce;
    static std::unordered_map<uint32_t, std::string> symbolMap;

    std::call_once(mapOnce, [] {
        // Torch writes this as a Blob at "assets/aBKAssetTable".
        // Format: u32 count, then for each entry: u32 assetId, s32 pathLen, char path[pathLen]
        auto res = Ship::Context::GetRawInstance()->GetResourceManager()->LoadResource("assets/aBKAssetTable");
        if (!res) {
            SPDLOG_ERROR("Failed to load asset manifest from o2r");
            return;
        }

        auto blob = std::dynamic_pointer_cast<Ship::Blob>(res);
        if (!blob || blob->Data.empty()) {
            SPDLOG_ERROR("Asset manifest blob is empty or wrong type");
            return;
        }

        const uint8_t* data = blob->Data.data();
        const size_t dataSize = blob->Data.size();
        size_t pos = 0;

        if (dataSize < 4) {
            SPDLOG_ERROR("Asset manifest too small");
            return;
        }

        uint32_t count;
        std::memcpy(&count, data + pos, 4);
        pos += 4;

        for (uint32_t i = 0; i < count && pos + 8 <= dataSize; i++) {
            uint32_t assetId;
            std::memcpy(&assetId, data + pos, 4);
            pos += 4;

            int32_t pathLen;
            std::memcpy(&pathLen, data + pos, 4);
            pos += 4;

            if (pathLen < 0 || pos + static_cast<size_t>(pathLen) > dataSize) {
                SPDLOG_ERROR("Asset manifest corrupt at entry {}", i);
                break;
            }

            std::string path(reinterpret_cast<const char*>(data + pos), static_cast<size_t>(pathLen));
            pos += static_cast<size_t>(pathLen);

            // pathLen == 0 marks an intentionally-empty ROM slot.
            if (path.empty()) {
                sEmptyAssetSlots.insert(assetId);
            } else {
                symbolMap[assetId] = std::move(path);
            }
        }

        // Non-v1.0 ROMs keep the SNS picture demos one slot lower (0x5E, matching
        // the PAL VER_SELECT in the decomp) and their map setups place the demo
        // entry points at that exit id too. The compiled v1.0 value (0x5F) would
        // load the right demo file via the alias but warp to a nonexistent exit,
        // so retarget the table; the 2508/2511/2513 aliases cover the asset side.
        if (Lighthouse::GetBaseVersion() != BK_VER_US_10) {
            for (auto& entry : D_80371F78) {
                entry.exit = 0x5E;
            }
        }
    });

    return symbolMap;
}

// True when assetId names a ROM slot that exists in the asset table but was never
// backed with data. Such a lookup is expected to return NULL (the game tolerates
// it, exactly as on N64) and must not be logged as a bad asset.
bool IsKnownEmptyAssetSlot(uint32_t assetId) {
    GetAssetSymbolMap(); // ensure the manifest (and empty-slot set) is parsed
    return sEmptyAssetSlots.count(assetId) != 0;
}
} // namespace

extern "C" int ResourceMgr_GetDialogLanguageCount(void) {
    return sDialogLanguageCount;
}

// PAL is the only base that carries more than one dialog language (EN/FR/DE).
extern "C" int ResourceMgr_IsPal(void) {
    return sDialogLanguageCount > 1 ? 1 : 0;
}

extern "C" int ResourceMgr_IsJapanese(void) {
    return sIsJapanese ? 1 : 0;
}

extern "C" int ResourceMgr_GetDialogLanguage(void) {
    return sDialogLanguage;
}

std::shared_ptr<Ship::IResource> GetResourceByName(const char* path) {
    try {
        return Ship::Context::GetRawInstance()->GetResourceManager()->LoadResource(path);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[ResourceHelpers] GetResourceByName('{}') exception: {}", path, e.what());
        return nullptr;
    }
}

// Keep shared_ptr references alive so raw pointers from GetResourceRawPointer
// don't become dangling when the resource manager evicts entries from its cache.
static std::unordered_map<uint32_t, std::shared_ptr<Ship::IResource>> sResourceRefCache;

#ifdef LUS_XBOX
// Banjo-Kazooie-X: the game's own count of who holds an asset, as the N64's asset cache kept it (anim_bonetransform.c:
// assetcache_get adds one, assetcache_release takes one), and which asset a pointer it was given is. An asset at none
// is unloaded at the next map change (ResourceMgr_XboxFreeUnused). One never counted (a port override, the intro's
// replacements) is never unloaded.
static std::recursive_mutex sAssetMutex;
static std::unordered_map<uint32_t, int32_t> sAssetUses;
static std::unordered_map<const void*, uint32_t> sAssetByPointer;
#define ASSET_LOCK std::lock_guard<std::recursive_mutex> assetLock(sAssetMutex)
#define ASSET_USED(id) (sAssetUses[(id)]++)
#else
#define ASSET_LOCK
#define ASSET_USED(id) ((void)0)
#endif

// Active dialog re-point: maps base (v1.0) dialog asset IDs to an alternate
// source archive's paths (a loaded language pack).
static std::unordered_map<uint32_t, std::string> sDialogOverride;

// The same, for a language pack scoped to the active romhack (mods/~lang/<hack>/).
static std::unordered_map<uint32_t, std::string> sScopedDialogOverride;

// Which language slot to read in this asset's blob. A hack's pack numbers its languages on its
// own, so its assets use its slot; everything else keeps the game's.
extern "C" int ResourceMgr_GetDialogSlot(uint32_t assetId, int defaultSlot) {
    return sScopedDialogOverride.count(assetId) != 0 ? sScopedDialogLanguage : defaultSlot;
}

// Active mod-overlay re-point: maps v1.0 asset IDs to a mod archive's own paths.
struct OverlayAsset {
    std::string archivePath;
    std::string path;
};
static std::unordered_map<uint32_t, OverlayAsset> sOverlayOverride;

// Bumped on every language change. This is the single "language changed" signal:
// the localization layer reads it to drive every live reaction: font slot, model
// re-fetch, file-select rebuild, etc
static int sLanguageGeneration = 0;

extern "C" int ResourceMgr_GetLanguageGeneration(void) {
    return sLanguageGeneration;
}

// 1 if the active language re-points this asset id (i.e. it resolves to a language
// pack rather than the base game). Used by the model reload to know which models
// must track the language.
extern "C" int ResourceMgr_IsAssetRepointed(uint32_t assetId) {
    return (sDialogOverride.count(assetId) != 0 || sScopedDialogOverride.count(assetId) != 0) ? 1 : 0;
}

// The base game's own o2r path for an asset id, ignoring any active language re-point.
std::string ResourceHelpers_GetBaseAssetPath(uint32_t assetId) {
    const auto& symbolMap = GetAssetSymbolMap();
    if (auto entry = symbolMap.find(assetId); entry != symbolMap.end()) {
        return entry->second;
    }
    return std::string();
}

std::string ResourceHelpers_GetActiveAssetPath(uint32_t assetId) {
    if (auto ov = sScopedDialogOverride.find(assetId); ov != sScopedDialogOverride.end()) {
        return ov->second;
    }
    if (auto ov = sOverlayOverride.find(assetId); ov != sOverlayOverride.end()) {
        return ov->second.path;
    }
    if (auto ov = sDialogOverride.find(assetId); ov != sDialogOverride.end()) {
        return ov->second;
    }
    return ResourceHelpers_GetBaseAssetPath(assetId);
}

bool ResourceHelpers_GetOverlayAsset(uint32_t assetId, std::string& outArchivePath, std::string& outPath) {
    auto it = sOverlayOverride.find(assetId);
    if (it == sOverlayOverride.end()) {
        return false;
    }
    outArchivePath = it->second.archivePath;
    outPath = it->second.path;
    return true;
}

void ResourceHelpers_BuildOverlayRepoints() {
    for (const auto& [id, path] : sOverlayOverride) {
        sResourceRefCache.erase(id);
    }
    sOverlayOverride.clear();

    auto archiveManager = Ship::Context::GetRawInstance()->GetResourceManager()->GetArchiveManager();
    auto archives = archiveManager->GetArchives();
    if (!archives) {
        return;
    }

    for (const auto& archive : *archives) {
        if (archive == nullptr || !archive->HasFile("assets/aGameConfig")) {
            continue;
        }
        auto files = archive->ListFiles();
        if (!files) {
            continue;
        }

        std::unordered_map<uint32_t, std::string> mine;
        for (const auto& [hash, pathName] : *files) {
            // (Banjo-Kazooie-X: the Xbox's archive index keeps the zip's own name, a const char*)
            const std::string path(pathName);
            if (path.rfind("assets/", 0) != 0 || path.find("lang/") != std::string::npos) {
                continue;
            }
            uint32_t id = 0;
            if (!Lighthouse::AssetHexFromPath(path, id)) {
                continue;
            }
            bool part = false;
            for (size_t cut = path.find('_', path.rfind("ASSET_") + 6); cut != std::string::npos && !part;
                 cut = path.find('_', cut + 1)) {
                part = archiveManager->HasFile(path.substr(0, cut));
            }
            if (part) {
                continue;
            }
            if (auto [it, inserted] = mine.try_emplace(id, path); !inserted && path.size() < it->second.size()) {
                it->second = path;
            }
        }

        for (auto& [id, path] : mine) {
            sResourceRefCache.erase(id);
            sOverlayOverride.insert_or_assign(id, OverlayAsset{ archive->GetPath(), std::move(path) });
        }
    }

    if (!sOverlayOverride.empty()) {
        SPDLOG_INFO("[ResourceHelpers] Re-pointed {} asset id(s) onto romhack overlays", sOverlayOverride.size());
    }
}

static char* LoadAndRetainResource(const std::string& path, uint32_t assetId) {
    auto res = GetResourceByName(path.c_str());
    if (res && res->GetRawPointer() != nullptr) {
        sResourceRefCache[assetId] = res;
#ifdef LUS_XBOX
        sAssetByPointer[res->GetRawPointer()] = assetId;
#endif
        return reinterpret_cast<char*>(res->GetRawPointer());
    }
    return nullptr;
}

// Reload an asset, evicting any cached version first.
// Used for map models whose vertex data gets modified at runtime.
extern "C" char* ResourceMgr_ReloadByAssetId(uint32_t assetId) {
    ASSET_LOCK;
    std::shared_ptr<Ship::IResource> oldRef;
    if (auto it = sResourceRefCache.find(assetId); it != sResourceRefCache.end()) {
        oldRef = std::move(it->second);
        sResourceRefCache.erase(it);
#ifdef LUS_XBOX
        sAssetByPointer.erase(oldRef->GetRawPointer());
#endif
    }

    std::string mappedPath = ResourceHelpers_GetActiveAssetPath(assetId);
    if (!mappedPath.empty()) {
        std::replace(mappedPath.begin(), mappedPath.end(), '\\', '/');

        // Unload from LUS cache so it re-reads from the o2r
        Ship::Context::GetRawInstance()->GetResourceManager()->UnloadResource(mappedPath);

        if (auto result = LoadAndRetainResource(mappedPath, assetId); result != nullptr) {
            ASSET_USED(assetId);
            return result;
        }
    }

    return nullptr;
}

extern "C" char* ResourceMgr_LoadByAssetId(uint32_t assetId) {
    ASSET_LOCK;
    // Return cached resource if already loaded
    if (auto it = sResourceRefCache.find(assetId); it != sResourceRefCache.end()) {
        auto ptr = it->second->GetRawPointer();
        if (ptr != nullptr) {
            ASSET_USED(assetId);
            return reinterpret_cast<char*>(ptr);
        }
        sResourceRefCache.erase(it);
    }

    std::string mappedPath = ResourceHelpers_GetActiveAssetPath(assetId);
    if (mappedPath.empty()) {
        // A reserved-but-empty ROM slot resolves to nothing by design (the game
        // tolerates it, as on N64); only a genuinely-missing asset is worth tracing.
        if (!IsKnownEmptyAssetSlot(assetId)) {
            port_traceBadAssetId(assetId);
        }
        return nullptr;
    }
    std::replace(mappedPath.begin(), mappedPath.end(), '\\', '/');

    if (auto result = LoadAndRetainResource(mappedPath, assetId); result != nullptr) {
        ASSET_USED(assetId);
        return result;
    }
    SPDLOG_WARN("[ResourceManager({})] symbol '{}' found but resource data is NULL", assetId, mappedPath);
    return nullptr;
}

// A track carries its own slot volume, read on first ask and kept.
extern "C" int32_t ResourceMgr_GetMusicTrackVolume(uint32_t assetId) {
    static std::unordered_map<uint32_t, int32_t> sVolumeCache;
    static std::mutex sVolumeMutex;
    {
        const std::lock_guard<std::mutex> lock(sVolumeMutex);
        if (auto it = sVolumeCache.find(assetId); it != sVolumeCache.end()) {
            return it->second;
        }
    }

    int32_t volume = -1;
    std::string path = ResourceHelpers_GetActiveAssetPath(assetId);
    if (!path.empty()) {
        std::replace(path.begin(), path.end(), '\\', '/');
        if (auto res = std::dynamic_pointer_cast<Factories::BKMusic>(GetResourceByName(path.c_str()))) {
            volume = static_cast<int32_t>(res->Volume);
        }
    }
    if (volume < 0) {
        SPDLOG_WARN("music: asset {} carries no slot volume; using the built-in table", assetId);
    }

    const std::lock_guard<std::mutex> lock(sVolumeMutex);
    sVolumeCache[assetId] = volume;
    return volume;
}

// Returns the data size of a previously loaded resource (from the ref cache).
// Used by decomp code that depends on assetCacheCurrentSize (e.g. demo_load).
extern "C" size_t ResourceMgr_GetResourceSize(uint32_t assetId) {
    if (auto it = sResourceRefCache.find(assetId); it != sResourceRefCache.end()) {
        return it->second->GetPointerSize();
    }
    return 0;
}

// Force an asset id to resolve to a custom resource shipped at `customPath`.
extern "C" void ResourceMgr_RegisterAssetOverride(uint32_t assetId, const char* customPath) {
    auto res = GetResourceByName(customPath);
    if (res != nullptr && res->GetRawPointer() != nullptr) {
        sResourceRefCache[assetId] = res;
    }
}

// Actors with sprite assets can be spawned as "model" props (unk8_1=1), causing collision
// code to call marker_loadModelBin which reinterprets sprite data as BKModelBin. This helper
// lets decomp code detect and skip the model path for sprite assets.
extern "C" int ResourceMgr_IsModelAsset(uint32_t assetId) {
    if (auto it = sResourceRefCache.find(assetId); it != sResourceRefCache.end()) {
        return it->second->GetInitData()->Type == 0x424B4D4F;
    }
    return 0;
}

extern "C" char* ResourceMgr_LoadTexOrDListByName(const char* filePath) {
    auto res = GetResourceByName(filePath);

    if (res->GetInitData()->Type == static_cast<uint32_t>(Fast::ResourceType::DisplayList))
        return (char*)&((std::static_pointer_cast<Fast::DisplayList>(res))->Instructions[0]);
    else {
        return (char*)ResourceGetDataByName(filePath);
    }
}

extern "C" Gfx* ResourceMgr_LoadGfxByName(const char* path) {
    auto res = std::static_pointer_cast<Fast::DisplayList>(GetResourceByName(path));
    return (Gfx*)&res->Instructions[0];
}

extern "C" Vtx* ResourceMgr_LoadVtxByName(char* path) {
    return (Vtx*)ResourceGetDataByName(path);
}

// Release all retained resource refs so destructors fire before spdlog shutdown
void ResourceHelpers_ClearRefCache() {
    ASSET_LOCK;
    sResourceRefCache.clear();
#ifdef LUS_XBOX
    sAssetByPointer.clear();
#endif
}

#ifdef LUS_XBOX
extern "C" void SpritePatches_ForgetSprite(const void* sprite); // Patches/SpritePatches.cpp
extern "C" int musicSlot_xboxUsesData(const void* data);       // core1/audio_instruments.c
static std::atomic<int> sXboxTexturesStale{ 0 };

// assetcache_release: one holder fewer. A pointer the cache didn't give out, or one already at none, changes nothing.
extern "C" void ResourceMgr_XboxReleaseAsset(const void* asset) {
    ASSET_LOCK;
    if (asset == nullptr) {
        return;
    }
    if (auto at = sAssetByPointer.find(asset); at != sAssetByPointer.end()) {
        if (auto uses = sAssetUses.find(at->second); uses != sAssetUses.end() && uses->second > 0) {
            uses->second--;
        }
    }
}

// A map changed (gameloop.c, game_changeMap, on the game's thread): the assets the game holds none of are unloaded,
// as the N64 freed them as their last holder let go; the other ports do the same at a scene change (Ocarina-X's
// ResourceMgr_XboxUnloadScene). With an asset go its sprite display data (SpritePatches) and its sub-resources in
// libultraship's cache (a model's _GEO and _VTX, a sprite's chunks and palettes: copied into it when it was built), and
// the renderer's texture cache is cleared before the next list is recorded (ResourceMgr_XboxTakeTexturesStale), its
// textures being keyed by address, where the next asset may be loaded.
extern "C" void ResourceMgr_XboxFreeUnused(void) {
    ASSET_LOCK;
    auto resources = Ship::Context::GetRawInstance()->GetResourceManager();
    size_t freed = 0, freedBytes = 0, kept = 0, keptBytes = 0, cacheLines = 0;

    for (auto it = sResourceRefCache.begin(); it != sResourceRefCache.end();) {
        const uint32_t id = it->first;
        auto uses = sAssetUses.find(id);
        const size_t bytes = it->second != nullptr ? it->second->GetPointerSize() : 0;
        const void* pointer = it->second != nullptr ? it->second->GetRawPointer() : nullptr;
        // (a music track a slot's sequence is still on: the slot holds it, though the game's count doesn't say so)
        if (uses == sAssetUses.end() || uses->second > 0 || musicSlot_xboxUsesData(pointer)) {
            kept++;
            keptBytes += bytes;
            ++it;
            continue;
        }
        SpritePatches_ForgetSprite(pointer);
        sAssetByPointer.erase(pointer);
        sAssetUses.erase(uses);
        std::string assetPath = ResourceHelpers_GetActiveAssetPath(id);
        std::replace(assetPath.begin(), assetPath.end(), '\\', '/');
        if (!assetPath.empty()) {
            cacheLines += resources->UnloadCachedUnder({ assetPath + "_" }, {});
            resources->UnloadResource(assetPath);
        }
        freed++;
        freedBytes += bytes;
        it = sResourceRefCache.erase(it);
    }
    if (freed > 0) {
        sXboxTexturesStale.store(1);
    }
    SPDLOG_INFO("lus-x: map change: {} assets nothing held unloaded ({} KB, {} parts of them), {} kept ({} KB); {} "
                "resources cached",
                freed, freedBytes >> 10, cacheLines, kept, keptBytes >> 10, resources->CachedResourceCount());
}

// the renderer's thread, before it records a list: true once after a sweep unloaded anything
extern "C" int ResourceMgr_XboxTakeTexturesStale(void) {
    return sXboxTexturesStale.exchange(0);
}
#endif

void ResourceHelpers_ApplyLanguage(std::unordered_map<uint32_t, std::string> dialogOverride,
                                   std::unordered_map<uint32_t, std::string> scopedDialogOverride, bool isJapanese,
                                   int dialogCount, int dialogIndex, int scopedDialogIndex) {
    auto evict = [](const std::unordered_map<uint32_t, std::string>& m) {
        for (const auto& [id, path] : m) {
            sResourceRefCache.erase(id);
        }
    };
    evict(sDialogOverride);
    evict(sScopedDialogOverride);
    sDialogOverride = std::move(dialogOverride);
    sScopedDialogOverride = std::move(scopedDialogOverride);
    evict(sDialogOverride);
    evict(sScopedDialogOverride);
    ++sLanguageGeneration; // invalidates re-pointed models on their next draw
    sIsJapanese = isJapanese;
    sDialogLanguageCount = (dialogCount > 0) ? dialogCount : 1;
    if (dialogIndex < 0 || dialogIndex >= sDialogLanguageCount) {
        SPDLOG_WARN("[ResourceHelpers] Dialog language index {} out of range for a {}-language source; using 0",
                    dialogIndex, sDialogLanguageCount);
        dialogIndex = 0;
    }
    sDialogLanguage = dialogIndex; // keep ResourceMgr_GetDialogLanguage() in sync
    sScopedDialogLanguage = (scopedDialogIndex >= 0) ? scopedDialogIndex : 0;
    func_8031B5C4(dialogIndex);
}
