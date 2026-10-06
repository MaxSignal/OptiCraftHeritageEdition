#include "net/minecraft/src/UiStrings.h"
#include "LegacyHelpOptions.h"

#include "LegacyGuiButton.h"
#include "LegacyControlsScreen.h"
#include "LegacyControllerLayoutScreen.h"
#include "platform/PlatformConfig.h"
#include "LegacyHeritageOptions.h"
#include "LegacyLanguageOptions.h"
#include "LegacyMainMenuLayout.h"
#include "LegacyVideoOptions.h"
#include "LegacyViewOptions.h"
#include "net/minecraft/src/GameSettings.h"
#include "net/minecraft/src/GuiTexturePacks.h"
#include "net/minecraft/src/Minecraft.h"

namespace
{
enum LegacyHelpButtonId
{
    BUTTON_VIDEO = 100,
    BUTTON_CONTROLS = 101,
    BUTTON_LANGUAGE = 102,
    BUTTON_HERITAGE = 103,
    BUTTON_VIEW = 104,
    BUTTON_TEXTURE_PACKS = 105,
    BUTTON_BACK = 200
};
}

LegacyHelpOptions::LegacyHelpOptions(GuiScreen *parent, GameSettings *settingsValue,
    LegacyOptionsBackgroundMode backgroundModeValue)
    : LegacyOptionsScreen(parent, settingsValue, backgroundModeValue)
{
}

void LegacyHelpOptions::initGui()
{
    // The TI-Nspire reads texture pack zips (TexturePackCustom); the Legacy
    // menus otherwise have no way to the texture pack screen.
#if PLATFORM_NSPIRE
    constexpr int_t count = 7;
#else
    constexpr int_t count = 6;
#endif
    configureLegacyLayout(count, false);
    const LegacyMainMenuLayout layout = legacyMainMenuLayout(width, height, count);
    const int_t stride = layout.buttonHeight + layout.buttonSpacing;
    const std::string labels[] = {
        uiText("Video"),
        uiText("Controls"),
        uiText("Language"),
        uiText("OptiCraft Options"),
        uiText("View"),
#if PLATFORM_NSPIRE
        uiText("Texture Packs"),
#endif
        uiText("Back")
    };
    const int_t ids[] = {
        BUTTON_VIDEO,
        BUTTON_CONTROLS,
        BUTTON_LANGUAGE,
        BUTTON_HERITAGE,
        BUTTON_VIEW,
#if PLATFORM_NSPIRE
        BUTTON_TEXTURE_PACKS,
#endif
        BUTTON_BACK
    };

    for (int_t i = 0; i < count; ++i)
    {
        controlList.push_back(new LegacyGuiButton(ids[i], layout.buttonX,
            layout.firstButtonY + i * stride, layout.buttonWidth, layout.buttonHeight, labels[i]));
    }
}

void LegacyHelpOptions::actionPerformed(GuiButton *button)
{
    if (button == nullptr || !button->enabled)
        return;

    settings->saveOptions();
    switch (button->id)
    {
    case BUTTON_VIDEO:
        mc->displayGuiScreen(new LegacyVideoOptions(this, settings, backgroundMode));
        return;
    case BUTTON_CONTROLS:
#if PLATFORM_PS2
        mc->displayGuiScreen(new LegacyControllerLayoutScreen(this, settings, backgroundMode));
#else
        mc->displayGuiScreen(new LegacyControlsScreen(this, settings, backgroundMode));
#endif
        return;
    case BUTTON_LANGUAGE:
        mc->displayGuiScreen(new LegacyLanguageOptions(this, settings, backgroundMode));
        return;
    case BUTTON_HERITAGE:
        mc->displayGuiScreen(new LegacyHeritageOptions(this, settings, backgroundMode));
        return;
    case BUTTON_VIEW:
        mc->displayGuiScreen(new LegacyViewOptions(this, settings, backgroundMode));
        return;
    case BUTTON_TEXTURE_PACKS:
        mc->displayGuiScreen(new GuiTexturePacks(this));
        return;
    case BUTTON_BACK:
        returnToParent();
        return;
    default:
        return;
    }
}

void LegacyHelpOptions::drawScreen(int_t mouseX, int_t mouseY, float_t partialTick)
{
    drawLegacyBackground(partialTick);
    updateLegacyPointerHover(mouseX, mouseY);
    GuiScreen::drawScreen(mouseX, mouseY, partialTick);
}
