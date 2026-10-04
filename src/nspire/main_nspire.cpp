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

namespace
{
int runGame(int argc, char** argv)
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
}

#ifndef _TINSPIRE
int main(int argc, char** argv) { return runGame(argc, argv); }
#else
// Ndless runs main() on the OS task's own stack, which is far smaller than the
// game's deepest paths (lighting and fluid updates, world generation, the GUI)
// need, and an overflow silently corrupts whatever lies below it. Run the game
// on a stack of its own from the heap. NspireSystem paints it so the status
// log can report how deep it actually went.
extern "C" int nspire_call_on_stack(void* top, int (*fn)(int, char**), int argc, char** argv);
asm(R"(
	.text
	.arm
	.align 2
	.global nspire_call_on_stack
nspire_call_on_stack:
	push {r4, lr}
	mov r4, sp
	mov sp, r0
	mov r12, r1
	mov r0, r2
	mov r1, r3
	blx r12
	mov sp, r4
	pop {r4, pc}
)");

int main(int argc, char** argv)
{
	void* top = NspireSystem::gameStackTop();
	if (top == nullptr)
		return runGame(argc, argv);
	return nspire_call_on_stack(top, runGame, argc, argv);
}
#endif

#endif
