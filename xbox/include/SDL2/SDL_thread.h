/* SDL2/SDL_thread.h -- Banjo-Kazooie-X: see SDL2/SDL.h (not SDL). Lighthouse names threads by SDL's id (its watchdog
 * and its message queues' waiters): the Xbox's own thread id (xbox_gamepad.cpp). */
#pragma once
#include "SDL2/SDL.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long SDL_threadID;
SDL_threadID SDL_ThreadID(void);

#ifdef __cplusplus
}
#endif
