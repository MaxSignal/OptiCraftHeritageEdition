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

// No FPU: every double operation is a soft-float library call at roughly twice
// the cost of the float one. Take every float path the codebase offers (the
// same switches the low-end PC profile uses); they trade the last bits of
// precision -- and, for the generators, small differences in a seed's world --
// for speed.
#undef  PLATFORM_FLOAT_VERTEX_MATH
#define PLATFORM_FLOAT_VERTEX_MATH       1
#undef  PLATFORM_FLOAT_ENTITY_AI_MATH
#define PLATFORM_FLOAT_ENTITY_AI_MATH    1
#undef  PLATFORM_FLOAT_ENTITY_CORE_MATH
#define PLATFORM_FLOAT_ENTITY_CORE_MATH  1
#undef  PLATFORM_FLOAT_FLUID_FLOW
#define PLATFORM_FLOAT_FLUID_FLOW        1
#undef  PLATFORM_FLOAT_EXPLOSION_MATH
#define PLATFORM_FLOAT_EXPLOSION_MATH    1
#undef  PLATFORM_FLOAT_VECTOR_MATH
#define PLATFORM_FLOAT_VECTOR_MATH       1
#undef  PLATFORM_FLOAT_COLLISION_SWEEP
#define PLATFORM_FLOAT_COLLISION_SWEEP   1
#undef  PLATFORM_FLOAT_ENTITY_DISTANCE
#define PLATFORM_FLOAT_ENTITY_DISTANCE   1
#undef  PLATFORM_FLOAT_CAVE_GENERATION
#define PLATFORM_FLOAT_CAVE_GENERATION   1
#undef  PLATFORM_FLOAT_TERRAIN_NOISE
#define PLATFORM_FLOAT_TERRAIN_NOISE     1
#undef  PLATFORM_FLOAT_BIOME_NOISE
#define PLATFORM_FLOAT_BIOME_NOISE       1
#undef  PLATFORM_FLOAT_ORE_VEINS
#define PLATFORM_FLOAT_ORE_VEINS         1

// Water, lava, fire and portal animations are computed in software floats every
// tick (~30 ms a tick on the calculator, measured on the title screen). Every
// two seconds still reads as moving water and keeps that hitch rare; at a
// quarter rate it was ~15 ms a frame.
#undef  PLATFORM_DYNAMIC_TEXTURE_INTERVAL_TICKS
#define PLATFORM_DYNAMIC_TEXTURE_INTERVAL_TICKS 40

// Terrain noise in integers: the heightmap generator's 2D noise
// (ChunkProviderGenerateLite.cpp) and the 3D Perlin octaves
// (NoiseGeneratorPerlin.cpp) run in Q16 fixed point.
#define PLATFORM_FIXED_TERRAIN_NOISE 1

// Per-tick work around the player, PS2-sized: random display ticks only spawn
// particles (1000 block probes a tick on the desktop), chunk lookups cached,
// no clouds / weather particles / camera effects, cheap particle physics.
#undef  PLATFORM_RANDOM_DISPLAY_PROBES
#define PLATFORM_RANDOM_DISPLAY_PROBES        64
#undef  PLATFORM_CACHE_RANDOM_DISPLAY_CHUNKS
#define PLATFORM_CACHE_RANDOM_DISPLAY_CHUNKS  1
#undef  PLATFORM_REUSE_RANDOM_DISPLAY_RNG
#define PLATFORM_REUSE_RANDOM_DISPLAY_RNG     1
#undef  PLATFORM_CACHE_RANDOM_TICK_CHUNKS
#define PLATFORM_CACHE_RANDOM_TICK_CHUNKS     1
#undef  PLATFORM_CACHE_SPAWN_CHUNKS
#define PLATFORM_CACHE_SPAWN_CHUNKS           1
#undef  PLATFORM_SKIP_CLOUDS
#define PLATFORM_SKIP_CLOUDS                  1
#undef  PLATFORM_SKIP_RAIN_SNOW
#define PLATFORM_SKIP_RAIN_SNOW               1
#undef  PLATFORM_SKIP_CAMERA_FX
#define PLATFORM_SKIP_CAMERA_FX               1
#undef  PLATFORM_FAST_PARTICLE_PHYSICS
#define PLATFORM_FAST_PARTICLE_PHYSICS        1
#undef  PLATFORM_MAX_PARTICLES_PER_LAYER
#define PLATFORM_MAX_PARTICLES_PER_LAYER      128
#undef  PLATFORM_CULL_MISSING_CHUNK_BOUNDARY_FACES
#define PLATFORM_CULL_MISSING_CHUNK_BOUNDARY_FACES 1
