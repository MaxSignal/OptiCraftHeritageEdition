// Display_nspire.cpp -- TI-Nspire implementation of lwjgl::Display.
//
// The screen is a fixed 320x240 RGB565 LCD; nGL renders into the back buffer
// NspireSystem owns and swapBuffers() blits it. "Close requested" maps to the
// home key (NspireKeypad), so quitting goes through the game's normal shutdown
// and saves the world.
#ifdef NSPIRE_PLATFORM

#include "lwjgl/Display.h"
#include "lwjgl/Mouse.h"

#include "client/Minecraft.h"
#include "net/minecraft/src/GuiScreen.h"
#include "nspire/NspireSystem.h"
#include "nspire/input/NspireKeypad.h"
#include "nspire/render/NglBackend.h"

namespace
{
bool g_created = false;
}

namespace lwjgl
{
namespace Display
{

void create()
{
	if (g_created) return;
	NglBackend::initialize();
	NspireKeypad::initialize(NspireSystem::kScreenWidth, NspireSystem::kScreenHeight);
	g_created = true;
}

void setDisplayMode(const DisplayMode &) {}

DisplayMode getDisplayMode()
{
	return DisplayMode(NspireSystem::kScreenWidth, NspireSystem::kScreenHeight);
}

void setTitle(const jstring &) {}
void setFullscreen(bool)       {}

bool isCloseRequested() { return NspireSystem::exitRequested(); }
bool isVisible()        { return true; }
bool isActive()         { return true; }

void processMessages()
{
	// Same reasoning as Display_wii.cpp: the input layer needs to know whether a
	// screen is open to decide what the arrows drive, and the game has to be
	// given focus back once a menu closes over a loaded world.
	Minecraft *mc = Minecraft::getMinecraft();
	const bool inMenu = (mc != nullptr && mc->currentScreen != nullptr);
	if (!inMenu && mc != nullptr && mc->theWorld != nullptr && !mc->inGameHasFocus)
		mc->setIngameFocus();

	NspireKeypad::poll(inMenu || !lwjgl::Mouse::isGrabbed());
}

void swapBuffers()
{
	NglBackend::present();
}

void update(bool doProcessMessages)
{
	swapBuffers();
	if (doProcessMessages)
		processMessages();
}

int_t getX() { return 0; }
int_t getY() { return 0; }
int_t getWidth()  { return NspireSystem::kScreenWidth; }
int_t getHeight() { return NspireSystem::kScreenHeight; }

} // namespace Display
} // namespace lwjgl

#endif // NSPIRE_PLATFORM
