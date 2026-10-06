#include "platform/Log.h"
#include "TexturePackList.h"

#include <algorithm>
#include <cctype>
#if !defined(PS2_PLATFORM) && !defined(WII_PLATFORM) && !defined(NSPIRE_PLATFORM)
#include <filesystem>
#endif
#include <iostream>
#include "TexturePackDefault.h"
#include "TexturePackCustom.h"
#include "Minecraft.h"
#include "GameSettings.h"
#include "java/String.h"
#if defined(NSPIRE_PLATFORM)
#include "platform/Storage.h"
#endif

#if !defined(PS2_PLATFORM) && !defined(WII_PLATFORM) && !defined(NSPIRE_PLATFORM)
namespace fs = std::filesystem;
#endif

TexturePackList::TexturePackList(Minecraft *minecraft, const std::string &file) :
	mc(minecraft),
	defaultTexturePack(new TexturePackDefault()),
	selectedTexturePack(nullptr)
{
	texturePackDir = file + "/texturepacks";
#if !defined(PS2_PLATFORM) && !defined(WII_PLATFORM) && !defined(NSPIRE_PLATFORM)
	if (!fs::exists(texturePackDir))
	{
		fs::create_directories(texturePackDir);
	}
#elif defined(NSPIRE_PLATFORM)
	// Created up front so the player finds where to copy packs to.
	if (!PlatformStorage::exists(texturePackDir))
		PlatformStorage::mkdirs(texturePackDir);
#endif
	currentTexturePack = minecraft->gameSettings->skin;
	updateAvailableTexturePacks();
	selectedTexturePack->loadTexturePack();
}

TexturePackList::~TexturePackList()
{
	if (selectedTexturePack != nullptr)
		selectedTexturePack->closeTexturePackFile();
	for (auto &entry : soundPool)
		delete entry.second;
	soundPool.clear();
	availableTexturePacks.clear();
	delete defaultTexturePack;
	defaultTexturePack = nullptr;
	selectedTexturePack = nullptr;
}

bool TexturePackList::setTexturePack(TexturePackBase *texturepackbase)
{
	if (texturepackbase == selectedTexturePack)
	{
		return false;
	}
	else
	{
		selectedTexturePack->closeTexturePackFile();
		currentTexturePack = texturepackbase->texturePackFileName;
		selectedTexturePack = texturepackbase;
		mc->gameSettings->skin = currentTexturePack;
		mc->gameSettings->saveOptions();
		selectedTexturePack->loadTexturePack();
		return true;
	}
}

void TexturePackList::updateAvailableTexturePacks()
{
	std::vector<TexturePackBase*> arraylist;
	selectedTexturePack = nullptr;
	arraylist.push_back(defaultTexturePack);

#if !defined(PS2_PLATFORM) && !defined(WII_PLATFORM) && !defined(NSPIRE_PLATFORM)
	if (fs::exists(texturePackDir) && fs::is_directory(texturePackDir))
	{
		for (const auto &entry : fs::directory_iterator(texturePackDir))
		{
			if (!entry.is_regular_file()) continue;
			std::string filename = entry.path().filename().string();
			const jstring lowerName = String::toLowerCaseJava(jstring(filename));
			if (lowerName.size() < 4 || lowerName.compare(lowerName.size() - 4, 4, ".zip") != 0) continue;

			auto file = entry.path();
			// libc++ represents file_time_type with rep __int128 and has not std::to_string for him (ambiguous call).
			const auto mtime = static_cast<long long>(fs::last_write_time(file).time_since_epoch().count());
			std::string s = filename + ":" + std::to_string(fs::file_size(file)) + ":" + std::to_string(mtime);

			try
			{
				auto it = soundPool.find(s);
				if (it == soundPool.end())
				{
					TexturePackCustom *texturepackcustom = new TexturePackCustom(file.string());
					texturepackcustom->texturePackFolder = s;
					soundPool[s] = texturepackcustom;
					texturepackcustom->getTexturePackFolder(mc);
				}
				TexturePackBase *texturepackbase1 = soundPool[s];
				if (texturepackbase1->texturePackFileName == currentTexturePack)
				{
					selectedTexturePack = texturepackbase1;
				}
				arraylist.push_back(texturepackbase1);
			}
			catch (std::exception &ioexception)
			{
				MC_LOG_ERROR("game", "%s\n", ioexception.what());
			}
		}
	}
#elif defined(NSPIRE_PLATFORM)
	// The calculator has no std::filesystem, so the folder is listed through
	// PlatformStorage. TI's transfer software only moves files whose names end
	// in .tns, so a pack copied as "MyPack.zip.tns" counts as "MyPack.zip".
	std::vector<std::string> names;
	if (PlatformStorage::listPathEntries(texturePackDir, names))
	{
		std::sort(names.begin(), names.end());
		for (const std::string &filename : names)
		{
			const std::string lowerName = String::toLowerCaseJava(jstring(filename));
			std::string packName = filename;
			if (lowerName.size() > 8 && lowerName.compare(lowerName.size() - 8, 8, ".zip.tns") == 0)
				packName = filename.substr(0, filename.size() - 4);
			else if (lowerName.size() < 4 || lowerName.compare(lowerName.size() - 4, 4, ".zip") != 0)
				continue;
			const std::string path = PlatformStorage::join(texturePackDir, filename);
			if (PlatformStorage::pathIsDirectory(path))
				continue;
			const std::string s = filename + ":" + std::to_string(PlatformStorage::getFileSize(path));

			auto it = soundPool.find(s);
			if (it == soundPool.end())
			{
				TexturePackCustom *texturepackcustom = new TexturePackCustom(path);
				texturepackcustom->texturePackFileName = packName;
				texturepackcustom->texturePackFolder = s;
				soundPool[s] = texturepackcustom;
				texturepackcustom->getTexturePackFolder(mc);
			}
			TexturePackBase *texturepackbase1 = soundPool[s];
			if (texturepackbase1->texturePackFileName == currentTexturePack)
				selectedTexturePack = texturepackbase1;
			arraylist.push_back(texturepackbase1);
		}
	}
#endif

	if (selectedTexturePack == nullptr)
	{
		selectedTexturePack = defaultTexturePack;
	}

	for (auto *tp : availableTexturePacks)
	{
		bool found = false;
		for (auto *tp2 : arraylist)
		{
			if (tp == tp2) { found = true; break; }
		}
		if (!found)
		{
			soundPool.erase(tp->texturePackFolder);
			delete tp;
		}
	}

	availableTexturePacks = arraylist;
}

const std::vector<TexturePackBase*> &TexturePackList::getAvailableTexturePacks() const
{
	return availableTexturePacks;
}

TexturePackBase *TexturePackList::getSelectedTexturePack() const
{
	return selectedTexturePack;
}
