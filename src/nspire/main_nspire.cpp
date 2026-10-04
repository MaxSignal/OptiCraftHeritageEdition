// TI-Nspire entry point (Ndless, or the desktop host simulator).
//
// Ndless starts the program with argv[0] set to the path of the launched .tns;
// NspireSystem derives the data directory from it. Minecraft::start() runs the
// whole game loop and returns when the player quits (or presses home).
#ifdef NSPIRE_PLATFORM

#include <exception>
#include <string>

#include "client/Minecraft.h"
#include "java/String.h"
#include "nspire/NspireSystem.h"
#include "nspire/render/NglBackend.h"

int main(int argc, char** argv)
{
	NspireSystem::initialize(argc, argv);
	NspireSystem::log("OptiCraft Heritage for TI-Nspire, built " __DATE__ " " __TIME__ "\n");

	try
	{
		jstring username = "Player";
		jstring auth = "-";
		Minecraft::start(&username, &auth);
	}
	catch (const std::exception& e)
	{
		NglBackend::shutdown();
		NspireSystem::fatal(std::string("Unhandled exception:\n") + e.what());
	}

	NglBackend::shutdown();
	NspireSystem::shutdown();
	return 0;
}

#endif
