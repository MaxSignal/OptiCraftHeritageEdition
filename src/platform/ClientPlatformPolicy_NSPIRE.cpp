#include "platform/ClientPlatformPolicy.h"

#include "net/minecraft/src/GameResources.h"
#include "net/minecraft/src/GameSettings.h"
#include "nspire/NspireSystem.h"

namespace ClientPlatformPolicy
{
int initialWidth()
{
    return NspireSystem::kScreenWidth;
}

int initialHeight()
{
    return NspireSystem::kScreenHeight;
}

std::string minecraftDirectory()
{
    return GameResources::getExeDir() + "/minecraft";
}

bool saveConverterUsesSavesSubdirectory()
{
    return true;
}

void applyGameSettingsDefaults(GameSettings* settings)
{
    if (settings == nullptr)
        return;

    // Everything here is pure fill-rate or CPU on a software rasteriser with no
    // FPU; none of it changes gameplay.
    settings->fancyGraphics = false;
    settings->ambientOcclusion = false;
    settings->particleSetting = 2;
    settings->ofVoidParticles = false;
    settings->ofWaterParticles = false;
    settings->ofRainSplash = false;
    settings->ofPortalParticles = false;
    settings->ofDrippingWaterLava = false;
    settings->ofWeather = false;
    settings->ofSky = false;
    settings->ofSunMoon = false;
    settings->ofClouds = 3;
}

void preloadStartupTextures(RenderEngine*)
{
}

void releaseWorldEntryAssets(RenderEngine*)
{
}

int panoramaSampleGrid()
{
    return 4;
}

void reportCrash(const std::string& description)
{
    NspireSystem::fatal(description);
}
}
