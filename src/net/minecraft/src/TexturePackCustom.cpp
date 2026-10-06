#include "platform/Log.h"
#include "TexturePackCustom.h"
#include "java/String.h"
#include "java/BufferedImage.h"

#include <iostream>
#include <algorithm>
#include <fstream>
#include <sstream>
#include "Minecraft.h"
#include "RenderEngine.h"
#include "platform/RenderAPI.h"

#if OPTICRAFT_TEXTURE_PACK_MINIZIP
#include <map>
#include "unzip.h"

// A standard texture pack zip, as a player downloads it. Its central directory
// is read once into `entries`, so a texture lookup seeks straight to the file
// instead of scanning the directory on the calculator's flash each time.
struct TexturePackCustom::ZipIndex
{
	unzFile file = nullptr;
	// Some packs wrap everything in one folder ("MyPack/terrain.png").
	std::string prefix;
	std::map<std::string, unz_file_pos> entries;

	~ZipIndex()
	{
		if (file != nullptr)
			unzClose(file);
	}

	static std::unique_ptr<ZipIndex> open(const std::string &path)
	{
		unzFile file = unzOpen(path.c_str());
		if (file == nullptr)
		{
			MC_LOG_ERROR("resources", "Failed to open texture pack: %s\n", path.c_str());
			return nullptr;
		}
		std::unique_ptr<ZipIndex> index(new ZipIndex());
		index->file = file;
		char name[512];
		for (int status = unzGoToFirstFile(file); status == UNZ_OK; status = unzGoToNextFile(file))
		{
			unz_file_info info;
			if (unzGetCurrentFileInfo(file, &info, name, sizeof(name), nullptr, 0, nullptr, 0) != UNZ_OK)
				continue;
			const std::string entry(name);
			if (entry.empty() || entry.back() == '/')
				continue;
			unz_file_pos position;
			if (unzGetFilePos(file, &position) == UNZ_OK)
				index->entries[entry] = position;
		}
		if (index->entries.count("terrain.png") == 0 && index->entries.count("pack.txt") == 0)
		{
			for (const auto &entry : index->entries)
			{
				const std::size_t slash = entry.first.find('/');
				if (slash == std::string::npos)
					continue;
				const std::string folder = entry.first.substr(0, slash + 1);
				if (index->entries.count(folder + "terrain.png") != 0 || index->entries.count(folder + "pack.txt") != 0)
				{
					index->prefix = folder;
					break;
				}
			}
		}
		return index;
	}

	// `name` relative to the pack root, without a leading '/'.
	bool read(const std::string &name, std::string &out)
	{
		const auto it = entries.find(prefix + name);
		if (it == entries.end())
			return false;
		unz_file_pos position = it->second;
		unz_file_info info;
		if (unzGoToFilePos(file, &position) != UNZ_OK ||
			unzGetCurrentFileInfo(file, &info, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK ||
			unzOpenCurrentFile(file) != UNZ_OK)
			return false;
		out.resize(info.uncompressed_size);
		const int bytesRead = info.uncompressed_size == 0 ? 0
			: unzReadCurrentFile(file, &out[0], static_cast<unsigned>(info.uncompressed_size));
		unzCloseCurrentFile(file);
		return bytesRead == static_cast<int>(info.uncompressed_size);
	}
};
#endif

TexturePackCustom::TexturePackCustom(const std::string &file) :
	texturePackZipFile(nullptr),
	texturePackName(-1),
	texturePackThumbnail(nullptr),
	texturePackFile(file)
{
	texturePackFileName = file;
}

TexturePackCustom::~TexturePackCustom() = default;

std::string TexturePackCustom::truncateString(const std::string &s)
{
	const jstring value(s);
	return String::utf16Length(value) > 34 ? String::substringUtf16(value, 0, 34) : value;
}

void TexturePackCustom::getTexturePackFolder(Minecraft *minecraft)
{
	(void)minecraft;
#ifdef MCBETA_HAVE_LIBZIP
	int err = 0;
	zip_t *zipfile = zip_open(texturePackFile.c_str(), 0, &err);
	if (!zipfile)
	{
		MC_LOG_ERROR("resources", "Failed to open texture pack: %s\n", texturePackFile.c_str());
		return;
	}

	// Read pack.txt
	zip_stat_t stat;
	if (zip_stat(zipfile, "pack.txt", 0, &stat) == 0)
	{
		zip_file_t *zf = zip_fopen(zipfile, "pack.txt", 0);
		if (zf)
		{
			std::vector<char> buf(stat.size + 1);
			zip_fread(zf, buf.data(), stat.size);
			buf[stat.size] = '\0';
			zip_fclose(zf);

			std::istringstream iss(buf.data());
			std::string line;
			if (std::getline(iss, line))
				firstDescriptionLine = truncateString(line);
			if (std::getline(iss, line))
				secondDescriptionLine = truncateString(line);
		}
	}

	// Read pack.png through the same image decoder used by the rest of Minecraft.
	texturePackThumbnail.reset();
	if (zip_stat(zipfile, "pack.png", 0, &stat) == 0)
	{
		zip_file_t *zf = zip_fopen(zipfile, "pack.png", 0);
		if (zf)
		{
			std::vector<char> buf(stat.size);
			const zip_int64_t bytesRead = zip_fread(zf, buf.data(), stat.size);
			zip_fclose(zf);
			if (bytesRead == static_cast<zip_int64_t>(stat.size))
			{
				try
				{
					std::istringstream imageStream(std::string(buf.data(), buf.size()), std::ios::in | std::ios::binary);
					texturePackThumbnail.reset(new BufferedImage(BufferedImage::ImageIO_read(imageStream)));
				}
				catch (...)
				{
					texturePackThumbnail.reset();
				}
			}
		}
	}

	zip_close(zipfile);
#elif OPTICRAFT_TEXTURE_PACK_MINIZIP
	std::unique_ptr<ZipIndex> index = ZipIndex::open(texturePackFile);
	if (index == nullptr)
		return;

	std::string text;
	if (index->read("pack.txt", text))
	{
		std::istringstream iss(text);
		std::string line;
		if (std::getline(iss, line))
			firstDescriptionLine = truncateString(line);
		if (std::getline(iss, line))
			secondDescriptionLine = truncateString(line);
	}

	texturePackThumbnail.reset();
	std::string image;
	if (index->read("pack.png", image))
	{
		try
		{
			std::istringstream imageStream(image, std::ios::in | std::ios::binary);
			texturePackThumbnail.reset(new BufferedImage(BufferedImage::ImageIO_read(imageStream)));
		}
		catch (...)
		{
			texturePackThumbnail.reset();
		}
	}
#endif
}

void TexturePackCustom::getResourceAsStream(Minecraft *minecraft)
{
	if (minecraft != nullptr && minecraft->renderEngine != nullptr && texturePackName >= 0)
	{
		minecraft->renderEngine->deleteTexture(texturePackName);
		texturePackName = -1;
	}
	closeTexturePackFile();
}

void TexturePackCustom::bindThumbnailTexture(Minecraft *minecraft)
{
	if (minecraft == nullptr || minecraft->renderEngine == nullptr)
		return;

#if defined(MCBETA_HAVE_LIBZIP) || OPTICRAFT_TEXTURE_PACK_MINIZIP
	if (texturePackThumbnail != nullptr && texturePackName < 0)
		texturePackName = minecraft->renderEngine->allocateAndSetupTexture(texturePackThumbnail.get());
	if (texturePackThumbnail != nullptr && texturePackName >= 0)
	{
		minecraft->renderEngine->bindTexture(texturePackName);
		return;
	}
#endif

	renderBindTexture(minecraft->renderEngine->getTexture("/gui/unknown_pack.png"));
}

void TexturePackCustom::loadTexturePack()
{
#ifdef MCBETA_HAVE_LIBZIP
	int err = 0;
	texturePackZipFile = zip_open(texturePackFile.c_str(), 0, &err);
#elif OPTICRAFT_TEXTURE_PACK_MINIZIP
	texturePackIndex = ZipIndex::open(texturePackFile);
#endif
}

void TexturePackCustom::closeTexturePackFile()
{
#ifdef MCBETA_HAVE_LIBZIP
	if (texturePackZipFile)
	{
		zip_close(texturePackZipFile);
		texturePackZipFile = nullptr;
	}
#elif OPTICRAFT_TEXTURE_PACK_MINIZIP
	texturePackIndex.reset();
#endif
}

std::istream* TexturePackCustom::getResourceAsStream(const std::string &s)
{
#ifdef MCBETA_HAVE_LIBZIP
	if (texturePackZipFile)
	{
		std::string entryName = s.substr(1); // remove leading /
		zip_stat_t stat;
		if (zip_stat(texturePackZipFile, entryName.c_str(), 0, &stat) == 0)
		{
			zip_file_t *zf = zip_fopen(texturePackZipFile, entryName.c_str(), 0);
			if (zf)
			{
				std::vector<char> buf(stat.size);
				zip_fread(zf, buf.data(), stat.size);
				zip_fclose(zf);
				return new std::istringstream(std::string(buf.data(), stat.size));
			}
		}
	}
#elif OPTICRAFT_TEXTURE_PACK_MINIZIP
	// Files the pack does not replace come from the game's own assets.
	std::string data;
	if (texturePackIndex != nullptr && !s.empty() && texturePackIndex->read(s.substr(1), data))
		return new std::istringstream(data, std::ios::in | std::ios::binary);
#endif
	return TexturePackBase::getResourceAsStream(s);
}

std::vector<std::string> TexturePackCustom::listResources(const std::string &prefix, const std::string &suffix)
{
#ifdef MCBETA_HAVE_LIBZIP
	std::vector<std::string> result;
	if (texturePackZipFile == nullptr)
		return result;
	std::string normalizedPrefix = prefix;
	while (!normalizedPrefix.empty() && normalizedPrefix.front() == '/')
		normalizedPrefix.erase(normalizedPrefix.begin());
	const zip_int64_t count = zip_get_num_entries(texturePackZipFile, 0);
	for (zip_uint64_t i = 0; i < static_cast<zip_uint64_t>(count); ++i)
	{
		const char *rawName = zip_get_name(texturePackZipFile, i, 0);
		if (rawName == nullptr)
			continue;
		const std::string name(rawName);
		if (name.rfind(normalizedPrefix, 0) != 0)
			continue;
		if (!suffix.empty() && (name.size() < suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0))
			continue;
		result.push_back('/' + name);
	}
	std::sort(result.begin(), result.end());
	return result;
#elif OPTICRAFT_TEXTURE_PACK_MINIZIP
	std::vector<std::string> result;
	if (texturePackIndex == nullptr)
		return result;
	std::string normalizedPrefix = prefix;
	while (!normalizedPrefix.empty() && normalizedPrefix.front() == '/')
		normalizedPrefix.erase(normalizedPrefix.begin());
	const std::string &root = texturePackIndex->prefix;
	for (const auto &entry : texturePackIndex->entries)
	{
		if (entry.first.compare(0, root.size(), root) != 0)
			continue;
		const std::string name = entry.first.substr(root.size());
		if (name.rfind(normalizedPrefix, 0) != 0)
			continue;
		if (!suffix.empty() && (name.size() < suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0))
			continue;
		result.push_back('/' + name);
	}
	return result;
#else
	return TexturePackBase::listResources(prefix, suffix);
#endif
}
