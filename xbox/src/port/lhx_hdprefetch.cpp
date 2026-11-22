// lhx_hdprefetch.cpp -- Banjo-Kazooie-X: a texture pack's HD textures read before they're drawn.
//
// A model's HD textures (alt/<its path>_tex_N, and their animation frames) were read from the disc the first time
// the renderer drew each one: a read, an inflate and a build in the middle of a frame, so walking into a new part of a
// level or turning the camera hitched for a frame or several. Now an asset the game loads (ResourceHelpers.cpp,
// LoadAndRetainResource) queues all of its HD textures, and they're read ahead of the renderer:
//
// - on a thread of their own at the lowest priority, so it reads while the game's threads wait (for the next blank,
//   the GPU, the disc) and never takes their time; one the renderer asks for first, it loads itself as before;
// - at a map change (ResourceMgr_XboxFreeUnused, the screen dark), the queue is finished on the game's thread: the
//   new map's own are all in before it's drawn, and none of the last map's is loaded after its assets are unloaded.
//
// They're kept as the renderer's own loads were: until the asset they belong to is unloaded at a map change.
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <libultraship/libultraship.h>
#include <spdlog/spdlog.h>

#include "ship/utils/StrHash64.h"

#include "lhx_thread.h"

extern "C" void LusXbox_LowerThisThread(void); // xbox_glue.cpp

namespace {

// an asset's path -> its HD textures' paths (the archive manager's own strings): built once, at the first asset
std::unordered_map<std::string, std::vector<const char*>> sIndex;
bool sIndexed = false;
std::mutex sIndexMutex;

std::mutex sQueueMutex;
std::condition_variable sQueueReady;
std::deque<const char*> sQueue;
bool sStarted = false;
std::mutex sLoadMutex; // held while one is loaded: a map change waits for the one in hand
uint32_t sQueued, sLoaded;

void BuildIndex(void) {
    auto archives = Ship::Context::GetRawInstance()->GetResourceManager()->GetArchiveManager();
    auto files = archives->ListFiles("alt/assets/*_tex_*");
    size_t textures = 0;

    for (const std::string& file : *files) {
        const size_t tex = file.rfind("_tex_");
        const char* path = archives->HashToCString(CRC64(file.c_str()));
        if (tex == std::string::npos || tex <= 4 || path == nullptr) {
            continue;
        }
        sIndex[file.substr(4, tex - 4)].push_back(path);
        textures++;
    }
    sIndexed = true;
    SPDLOG_INFO("lus-x: HD textures read ahead: {} of {} assets", textures, sIndex.size());
}

void LoadOne(const char* path) {
    const std::lock_guard<std::mutex> lock(sLoadMutex);
    Ship::Context::GetRawInstance()->GetResourceManager()->LoadResource(std::string(path));
    sLoaded++;
}

const char* Take(void) {
    const std::lock_guard<std::mutex> lock(sQueueMutex);
    if (sQueue.empty()) {
        return nullptr;
    }
    const char* path = sQueue.front();
    sQueue.pop_front();
    return path;
}

void Worker(void) {
    LusXbox_LowerThisThread();
    for (;;) {
        const char* path;
        {
            std::unique_lock<std::mutex> lock(sQueueMutex);
            sQueueReady.wait(lock, [] { return !sQueue.empty(); });
            path = sQueue.front();
            sQueue.pop_front();
        }
        LoadOne(path);
    }
}

} // namespace

// an asset the game has just loaded (its path in the archives): its HD textures queued
void HdPrefetch_Asset(const std::string& assetPath) {
    auto resources = Ship::Context::GetRawInstance()->GetResourceManager();
    if (!resources->IsAltAssetsEnabled()) {
        return;
    }
    const std::vector<const char*>* paths;
    {
        const std::lock_guard<std::mutex> lock(sIndexMutex);
        if (!sIndexed) {
            BuildIndex();
        }
        const auto found = sIndex.find(assetPath);
        if (found == sIndex.end()) {
            return;
        }
        paths = &found->second; // (the index never changes once built)
    }
    {
        const std::lock_guard<std::mutex> lock(sQueueMutex);
        sQueue.insert(sQueue.end(), paths->begin(), paths->end());
        sQueued += (uint32_t)paths->size();
        if (!sStarted) {
            sStarted = true;
            LHX_THREAD(LHX_THREAD_STACK(64 * 1024) Worker).detach(); // for as long as the game runs
        }
    }
    sQueueReady.notify_one();
}

// a map change, on the game's thread: the rest of the queue loaded here, and the one the thread has in hand finished
void HdPrefetch_Finish(void) {
    uint32_t here = 0;
    for (const char* path = Take(); path != nullptr; path = Take()) {
        LoadOne(path);
        here++;
    }
    const std::lock_guard<std::mutex> wait(sLoadMutex);
    if (sQueued > 0) {
        SPDLOG_INFO("lus-x: HD textures read ahead: {} queued since the last map, {} read ({} of them at the change)",
                    sQueued, sLoaded, here);
    }
    sQueued = sLoaded = 0;
}
