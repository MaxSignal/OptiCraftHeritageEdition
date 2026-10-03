#pragma once

// -----------------------------------------------------------------------------
// TI-Nspire overrides
// -----------------------------------------------------------------------------
// Mirrors PlatformWiiTuning.h: the Nspire takes the desktop branch of the tables
// and then overrides the values that assume desktop memory and a GPU.
#if PLATFORM_NSPIRE
#  include "nspire/NspireTuning.h"
#  undef  PLATFORM_LEGACY_GUI_SCALE
#  define PLATFORM_LEGACY_GUI_SCALE NSPIRE_LEGACY_GUI_SCALE
#  undef  PLATFORM_LEGACY_CREATE_WORLD_PANEL_WIDTH
#  define PLATFORM_LEGACY_CREATE_WORLD_PANEL_WIDTH NSPIRE_LEGACY_CREATE_WORLD_PANEL_WIDTH
#endif
