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

// Movement on B / H / I / J: on the calculator's letter grid (ABCDEFG over
// HIJKLMN) they sit like W / A / S / D on a PC keyboard, whereas W, A, S and D
// are scattered across three rows.
namespace
{
struct MovementKey
{
	KeyBinding *GameSettings::*binding;
	int_t desktopKey;
	int_t nspireKey;
};
const MovementKey kMovementKeys[] = {
	{&GameSettings::keyBindForward, lwjgl::Keyboard::KEY_W, lwjgl::Keyboard::KEY_B},
	{&GameSettings::keyBindLeft, lwjgl::Keyboard::KEY_A, lwjgl::Keyboard::KEY_H},
	{&GameSettings::keyBindBack, lwjgl::Keyboard::KEY_S, lwjgl::Keyboard::KEY_I},
	{&GameSettings::keyBindRight, lwjgl::Keyboard::KEY_D, lwjgl::Keyboard::KEY_J},
};

// Desktop defaults (fresh settings, or an options file saved by an older
// build) become the calculator layout; a key the player chose stays.
void applyMovementKeys(GameSettings& settings)
{
	bool changed = false;
	for (const MovementKey& m : kMovementKeys)
	{
		KeyBinding *binding = settings.*(m.binding);
		if (binding != nullptr && binding->keyCode == m.desktopKey)
		{
			binding->keyCode = m.nspireKey;
			changed = true;
		}
	}
	if (changed)
		KeyBinding::resetKeyBindingArrayAndHash();
}
}

void platformGameSettingsInitialize(GameSettings& settings)
{
	settings.renderDistance = PLATFORM_DEFAULT_RENDER_DISTANCE;
	platformGameSettingsApplyLegacyCrafting(settings);
	applyMovementKeys(settings);
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
// across), so it must follow the Tiny/Short choice: one chunk (16 blocks, a
// 3x3 column grid) or two (32, 5x5). Left at the desktop default of 128 it
// built a 17x17 column grid, ~1150 sections frustum-tested and scheduled every
// frame for a world that only streams a few chunks around the player. On a
// 320x240 screen at a few frames per second, one chunk is the playable default.
int_t platformGameSettingsClampFineRenderDistance(int_t value)
{
	const int_t maxDistance = 32;
	return value < 16 ? 16 : (value > maxDistance ? maxDistance : value);
}

void platformGameSettingsUpdateRenderDistanceFromFine(int_t fineDistance, int_t& renderDistance)
{
	fineDistance = platformGameSettingsClampFineRenderDistance(fineDistance);
	renderDistance = fineDistance > 16 ? 2 : 3;
}

bool platformGameSettingsAnaglyphValue(bool, bool) { return false; }

// Texture animations (water, lava, fire, portal, redstone, explosion, flame,
// smoke and texture-pack animations) default to off: each is a soft-float
// pass over its tiles, ~30 ms per update on the calculator. Applied once to
// fresh settings and to option files from builds before this default; the
// marker key keeps a player's later choice in the options screen.
namespace
{
constexpr const char* kQuietDefaultsKey = "nspireQuietDefaults";
bool g_quietDefaultsApplied = false;

void applyQuietDefaults(GameSettings& settings)
{
	settings.ofAnimatedWater = 2;
	settings.ofAnimatedLava = 2;
	settings.ofAnimatedFire = false;
	settings.ofAnimatedPortal = false;
	settings.ofAnimatedRedstone = false;
	settings.ofAnimatedExplosion = false;
	settings.ofAnimatedFlame = false;
	settings.ofAnimatedSmoke = false;
	settings.ofAnimatedTextures = false;
	settings.particleSetting = 2;
	g_quietDefaultsApplied = true;
}
}

bool platformGameSettingsLoadOption(GameSettings&, const std::string& key, const std::string& value)
{
	if (key != kQuietDefaultsKey)
		return false;
	g_quietDefaultsApplied = value == "true";
	return true;
}

void platformGameSettingsFinalizeLoad(GameSettings& settings)
{
	settings.renderDistance = platformGameSettingsClampRenderDistance(settings.renderDistance);
	// GameSettings::setDefaults() assigns the desktop fine distance after
	// platformGameSettingsInitialize(), and older option files saved it.
	settings.ofRenderDistanceFine = platformGameSettingsClampFineRenderDistance(
		16 << (3 - settings.renderDistance));
	settings.ofChunkUpdates = std::max(settings.ofChunkUpdates, (int_t)1);
	platformGameSettingsApplyLegacyCrafting(settings);
	applyMovementKeys(settings);
	if (!g_quietDefaultsApplied)
		applyQuietDefaults(settings);
}
void platformGameSettingsSyncControllerBindings(const GameSettings&) {}
void platformGameSettingsAddKnownKeys(std::unordered_set<std::string>& keys) { keys.insert(kQuietDefaultsKey); }
void platformGameSettingsWriteOptions(const GameSettings&, std::ostream& out)
{
	out << kQuietDefaultsKey << ":" << (g_quietDefaultsApplied ? "true" : "false") << "\n";
}
