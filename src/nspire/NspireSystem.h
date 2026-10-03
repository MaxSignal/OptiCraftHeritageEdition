#pragma once

// TI-Nspire system layer: clock, framebuffer, keypad scan, logging and the
// install directory. Two implementations share this interface:
//
//   NspireSystem_device.cpp   Ndless on the calculator (_TINSPIRE).
//   NspireSystem_hostsim.cpp  A headless desktop build of the same port, used
//                             to run the game logic and the nGL renderer on a
//                             PC: frames are dumped as PPM files and the
//                             keypad is driven from a script.
//
// Everything above this layer (lwjgl shims, nGL backend, storage) is identical
// in both builds.

#include <cstdint>
#include <string>

namespace NspireSystem
{
constexpr int kScreenWidth = 320;
constexpr int kScreenHeight = 240;

// argv[0] is the full path of the launched .tns on the calculator.
void initialize(int argc, char** argv);
void shutdown();

// Directory that holds the .tns and the game data (assets.pak.tns, saves).
const std::string& appDir();

// Monotonic clock driven by a hardware timer, not newlib's 1 s RTC.
std::uint64_t micros();
std::uint32_t millis();
void delayMs(std::uint32_t ms);

// RGB565 back buffer that nGL renders into, and its presentation.
std::uint16_t* backBuffer();
void present();

// Keypad. Key ids are NspireKey values (see input/NspireKeypad.h). On the
// calculator the touchpad's edges answer as the arrow keys (libndls does that
// inside isKeyPressed), so models with and without a touchpad read the same.
void scanKeys();
bool keyDown(int key);

bool exitRequested();
void requestExit();

long heapFreeKb();

void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
[[noreturn]] void fatal(const std::string& message);
}
