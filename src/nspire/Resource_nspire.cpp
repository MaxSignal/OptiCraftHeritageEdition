// Resource_nspire.cpp -- TI-Nspire implementation of Resource::getResource().
//
// Resources come from assets.pak.tns next to the program (or a loose
// data/assets tree), resolved by PlatformResources. Callers ask for paths
// rooted at the resource directory ("/terrain.png"); the stream is owned by the
// caller, as on every other platform.
#ifdef NSPIRE_PLATFORM

#include "java/Resource.h"
#include "java/String.h"
#include "net/minecraft/src/GameResources.h"
#include "platform/storage/PathUtils.h"

#include <stdexcept>
#include <string>

namespace Resource
{

std::istream *getResource(const jstring &name)
{
	auto input = GameResources::open(static_cast<const std::string &>(name));
	if (!input)
	{
		const std::string path = PlatformStorage::join(
			GameResources::getAssetsDir(), static_cast<const std::string &>(name));
		throw std::runtime_error(
			"Missing game resource:\n" + path +
			"\n\nCopy assets.pak to the calculator as\n"
			"assets.pak.tns, in the same folder as\n"
			"opticraft.tns.");
	}

	return input.release();
}

} // namespace Resource

#endif // NSPIRE_PLATFORM
