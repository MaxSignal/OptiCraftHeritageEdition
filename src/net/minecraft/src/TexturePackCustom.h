#pragma once

#include "TexturePackBase.h"
#include "platform/PlatformConfig.h"
#include <memory>
#include <string>

// Texture pack zips through the bundled minizip where libzip is absent. Set
// here rather than per target because it changes this class's members.
#ifndef OPTICRAFT_TEXTURE_PACK_MINIZIP
#  if PLATFORM_NSPIRE && !defined(MCBETA_HAVE_LIBZIP)
#    define OPTICRAFT_TEXTURE_PACK_MINIZIP 1
#  else
#    define OPTICRAFT_TEXTURE_PACK_MINIZIP 0
#  endif
#endif

#ifdef MCBETA_HAVE_LIBZIP
#include <zip.h>
#else
struct zip;
typedef struct zip zip_t;
#endif

class BufferedImage;
class Minecraft;

// net.minecraft.src.TexturePackCustom
class TexturePackCustom : public TexturePackBase
{
public:
	TexturePackCustom(const std::string &file);
	~TexturePackCustom() override;

	void getTexturePackFolder(Minecraft *minecraft) override;
	void getResourceAsStream(Minecraft *minecraft) override;
	void bindThumbnailTexture(Minecraft *minecraft) override;
	void loadTexturePack() override;
	void closeTexturePackFile() override;
	std::istream* getResourceAsStream(const std::string &s) override;
	std::vector<std::string> listResources(const std::string &prefix, const std::string &suffix) override;

private:
	std::string truncateString(const std::string &s);

#if OPTICRAFT_TEXTURE_PACK_MINIZIP
	// Without libzip (the TI-Nspire build): the zip read through the bundled
	// minizip, with its entry table indexed once when the pack is opened.
	struct ZipIndex;
	std::unique_ptr<ZipIndex> texturePackIndex;
#endif
	zip_t *texturePackZipFile;
	int_t texturePackName; // Renderer texture handle
	std::unique_ptr<BufferedImage> texturePackThumbnail;
	std::string texturePackFile;
};
