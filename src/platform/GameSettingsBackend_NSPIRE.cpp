#include "platform/GameSettingsBackend.h"

#include <algorithm>
#include <ostream>

#include "platform/PlatformConfig.h"
#include "platform/PlatformTuning.h"

#include "lwjgl/Keyboard.h"
#include "net/minecraft/src/GameSettings.h"
#include "net/minecraft/src/KeyBinding.h"

// Keyboard bindings are the desktop ones (the keypad emits lwjgl key codes).
// Render distance is held to Tiny..Short like the Wii, starting at Tiny: the
// resident chunk window (NspireWorldTuning.h) cannot feed anything wider.
void platformGameSettingsApplyLegacyCrafting(GameSettings& settings)
{
	if (settings.keyBindCrafting != nullptr)
	{
		if (settings.legacyCrafting)
		{
			if (settings.keyBindCrafting->keyCode == 0)
				settings.keyBindCrafting->keyCode = lwjgl::Keyboard::KEY_C;
		}
		else
		{
			settings.keyBindCrafting->keyCode = 0;
		}
	}
}

void platformGameSettingsInitialize(GameSettings& settings)
{
	settings.renderDistance = PLATFORM_DEFAULT_RENDER_DISTANCE;
	platformGameSettingsApplyLegacyCrafting(settings);
}
void platformGameSettingsResetControlBindings(GameSettings&) {}
int_t platformGameSettingsDefaultChunkUpdates() { return (int_t)PLATFORM_MAX_RENDERER_UPDATES_PER_FRAME; }
int_t platformGameSettingsDefaultConnectedTextures() { return 3; }

int_t platformGameSettingsCycleRenderDistance(int_t current, int_t delta)
{
	const int_t lowest = 2;
	const int_t span = 3 - lowest + 1;
	current += delta;
	while (current > 3) current -= span;
	while (current < lowest) current += span;
	return current;
}

int_t platformGameSettingsClampRenderDistance(int_t value)
{
	return value < 2 ? 2 : (value > 3 ? 3 : value);
}

// The renderer grid is built from the fine distance (2 * fine / 16 + 1 sections
// across), so it must follow the Tiny/Short choice: 32 or 64 blocks. Left at
// the desktop default of 128 it built a 17x17 column grid, ~1150 sections
// frustum-tested and scheduled every frame for a world that only streams a few
// chunks around the player.
int_t platformGameSettingsClampFineRenderDistance(int_t value)
{
	const int_t maxDistance = 64;
	return value < 32 ? 32 : (value > maxDistance ? maxDistance : value);
}

void platformGameSettingsUpdateRenderDistanceFromFine(int_t fineDistance, int_t& renderDistance)
{
	fineDistance = platformGameSettingsClampFineRenderDistance(fineDistance);
	renderDistance = fineDistance > 32 ? 2 : 3;
}

bool platformGameSettingsAnaglyphValue(bool, bool) { return false; }
bool platformGameSettingsLoadOption(GameSettings&, const std::string&, const std::string&) { return false; }

void platformGameSettingsFinalizeLoad(GameSettings& settings)
{
	settings.renderDistance = platformGameSettingsClampRenderDistance(settings.renderDistance);
	// GameSettings::setDefaults() assigns the desktop fine distance after
	// platformGameSettingsInitialize(), and older option files saved it.
	settings.ofRenderDistanceFine = platformGameSettingsClampFineRenderDistance(
		32 << (3 - settings.renderDistance));
	settings.ofChunkUpdates = std::max(settings.ofChunkUpdates, (int_t)1);
	platformGameSettingsApplyLegacyCrafting(settings);
}
void platformGameSettingsSyncControllerBindings(const GameSettings&) {}
void platformGameSettingsAddKnownKeys(std::unordered_set<std::string>&) {}
void platformGameSettingsWriteOptions(const GameSettings&, std::ostream&) {}
