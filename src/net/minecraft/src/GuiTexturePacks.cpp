#include "GuiTexturePacks.h"
#include "GuiTexturePackSlot.h"
#include "GuiSmallButton.h"
#include "GuiButton.h"
#include "StringTranslate.h"
#include "TexturePackList.h"
#include "RenderEngine.h"
#include "FontRenderer.h"
#include "Minecraft.h"
#include "java/File.h"
#include "java/System.h"
#include "java/String.h"
#include <memory>
#ifdef NSPIRE_PLATFORM
#include "pc/lwjgl/Keyboard.h"
#endif

GuiTexturePacks::GuiTexturePacks(GuiScreen *guiscreen)
	: field_6454_o(-1)
	, fileLocation("")
	, guiScreen(guiscreen)
	, guiTexturePackSlot(nullptr)
{
}

GuiTexturePacks::~GuiTexturePacks()
{
	delete guiTexturePackSlot;
}

void GuiTexturePacks::initGui()
{
	StringTranslate *stringtranslate = StringTranslate::getInstance();
	controlList.push_back(new GuiSmallButton(5, width / 2 - 154, height - 48, stringtranslate->translateKey("texturePack.openFolder")));
#ifdef NSPIRE_PLATFORM
	// No file browser to open on the calculator; packs are copied to
	// minecraft/texturepacks with TI's transfer software.
	controlList.back()->enabled = false;
#endif
	controlList.push_back(new GuiSmallButton(6, width / 2 + 4,   height - 48, stringtranslate->translateKey("gui.done")));
	mc->texturePackList->updateAvailableTexturePacks();
	std::unique_ptr<File> texturePackDirectory(File::open(*Minecraft::getMinecraftDir(), "texturepacks"));
	fileLocation = texturePackDirectory->toString();
	delete guiTexturePackSlot;
	guiTexturePackSlot = new GuiTexturePackSlot(this);
	guiTexturePackSlot->registerScrollButtons(controlList, 7, 8);
}

void GuiTexturePacks::actionPerformed(GuiButton *guibutton)
{
	if (!guibutton->enabled) return;
	if (guibutton->id == 5)
	{
		System::openURL("file://" + fileLocation);
	}
	else if (guibutton->id == 6)
	{
		mc->refreshResources();
		mc->displayGuiScreen(guiScreen);
	}
	else
	{
		guiTexturePackSlot->actionPerformed(guibutton);
	}
}

#ifdef NSPIRE_PLATFORM
// The pack list is a mouse-only GuiSlot, and the arrows would only walk the
// two buttons under it. On the calculator the arrows pick the pack instead and
// enter or esc applies it, reloading the textures once rather than per step.
bool GuiTexturePacks::usesSpecializedMenuNavigation() const
{
	return true;
}

void GuiTexturePacks::keyTyped(char_t c, int_t key)
{
	(void)c;
	if (key == lwjgl::Keyboard::KEY_UP || key == lwjgl::Keyboard::KEY_DOWN)
	{
		const auto &packs = mc->texturePackList->getAvailableTexturePacks();
		if (packs.empty())
			return;
		int_t current = 0;
		for (std::size_t i = 0; i < packs.size(); ++i)
		{
			if (packs[i] == mc->texturePackList->getSelectedTexturePack())
				current = static_cast<int_t>(i);
		}
		const int_t count = static_cast<int_t>(packs.size());
		const int_t next = (current + (key == lwjgl::Keyboard::KEY_DOWN ? 1 : count - 1)) % count;
		mc->texturePackList->setTexturePack(packs[static_cast<std::size_t>(next)]);
	}
	else if (key == lwjgl::Keyboard::KEY_RETURN || key == lwjgl::Keyboard::KEY_ESCAPE)
	{
		mc->refreshResources();
		mc->displayGuiScreen(guiScreen);
	}
}
#endif

void GuiTexturePacks::mouseClicked(int_t i, int_t j, int_t k)
{
	GuiScreen::mouseClicked(i, j, k);
}

void GuiTexturePacks::mouseMovedOrUp(int_t i, int_t j, int_t k)
{
	GuiScreen::mouseMovedOrUp(i, j, k);
}

void GuiTexturePacks::drawScreen(int_t i, int_t j, float_t f)
{
	guiTexturePackSlot->drawScreen(i, j, f);
	if (field_6454_o <= 0)
	{
		mc->texturePackList->updateAvailableTexturePacks();
		field_6454_o += 20;
	}
	StringTranslate *stringtranslate = StringTranslate::getInstance();
	drawCenteredString(fontRenderer, stringtranslate->translateKey("texturePack.title"),      width / 2,      16,            0xffffff);
	drawCenteredString(fontRenderer, stringtranslate->translateKey("texturePack.folderInfo"), width / 2 - 77, height - 26,   0x808080);
	GuiScreen::drawScreen(i, j, f);
}

void GuiTexturePacks::updateScreen()
{
	GuiScreen::updateScreen();
	field_6454_o--;
}
