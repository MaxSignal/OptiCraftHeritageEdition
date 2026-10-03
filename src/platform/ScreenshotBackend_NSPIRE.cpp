#include "ScreenshotBackend.h"

#include <cstdio>
#include <vector>

#include "nspire/NspireSystem.h"
#include "platform/storage/PathUtils.h"
#include "platform/storage/PosixFileSystem.h"

// F2 on the desktop. The calculator has no PNG viewer and TI's software only
// transfers .tns files, so the frame is written as a binary PPM named *.ppm.tns:
// strip the .tns after copying it off the calculator and any image tool opens it.
namespace ScreenshotBackend
{
std::string save(const std::string &basePath, int_t width, int_t height)
{
    (void)width;
    (void)height;
    const std::string dir = PlatformStorage::join(basePath, "screenshots");
    PlatformStorage::makeDirectories(dir);

    std::string path;
    for (int i = 1; i < 1000; ++i)
    {
        char name[32];
        std::snprintf(name, sizeof(name), "shot_%03d.ppm.tns", i);
        path = PlatformStorage::join(dir, name);
        if (!PlatformStorage::posixExists(path))
            break;
    }

    const int w = NspireSystem::kScreenWidth;
    const int h = NspireSystem::kScreenHeight;
    const std::uint16_t* src = NspireSystem::backBuffer();
    std::vector<unsigned char> rgb(static_cast<std::size_t>(w) * h * 3);
    for (int i = 0; i < w * h; ++i)
    {
        const std::uint16_t c = src[i];
        rgb[i * 3 + 0] = static_cast<unsigned char>(((c >> 11) & 31) * 255 / 31);
        rgb[i * 3 + 1] = static_cast<unsigned char>(((c >> 5) & 63) * 255 / 63);
        rgb[i * 3 + 2] = static_cast<unsigned char>((c & 31) * 255 / 31);
    }

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr)
        return "Failed to save screenshot";
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::fwrite(rgb.data(), 1, rgb.size(), f);
    std::fclose(f);
    return "Saved screenshot as " + path;
}
}
