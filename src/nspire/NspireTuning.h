#pragma once

// TI-Nspire budgets. Same shape as wii/WiiTuning.h: the desktop table in
// PlatformGameTuning.h is the baseline and these files override what a 64 MB,
// FPU-less, GPU-less calculator cannot afford. Values start from the Wii set
// and are shrunk for a 3x3 resident chunk window and a 320x240 screen.
#include "nspire/tuning/NspireWorldTuning.h"
#include "nspire/tuning/NspireFrameTuning.h"
#include "nspire/tuning/NspireGameplayTuning.h"
#include "nspire/tuning/NspireStreamingTuning.h"

// The section-build state machine and mesh-cache eviction are shared with the
// Wii (wii/minecraft/WorldRendererWii.cpp, RenderGlobalWii.cpp) and read their
// knobs under the Wii names. Here the "GX list" budget is the RAM held by the
// captured terrain meshes nGL replays.
#define PLATFORM_WII_CHUNK_BUILD_BLOCKS_PER_STEP           PLATFORM_NSPIRE_CHUNK_BUILD_BLOCKS_PER_STEP
#define PLATFORM_WII_CHUNK_BUILD_STEP_US                   PLATFORM_NSPIRE_CHUNK_BUILD_STEP_US
#define PLATFORM_WII_GXLIST_EVICT_HIGH_WATER_BYTES         PLATFORM_NSPIRE_GXLIST_EVICT_HIGH_WATER_BYTES
#define PLATFORM_WII_GXLIST_EVICT_KEEP_RADIUS_BLOCKS       PLATFORM_NSPIRE_GXLIST_EVICT_KEEP_RADIUS_BLOCKS
#define PLATFORM_WII_GXLIST_EVICT_MAX_PER_FRAME            PLATFORM_NSPIRE_GXLIST_EVICT_MAX_PER_FRAME
#define PLATFORM_WII_RENDERER_DEPENDENCY_REQUESTS_PER_STEP PLATFORM_NSPIRE_RENDERER_DEPENDENCY_REQUESTS_PER_STEP
