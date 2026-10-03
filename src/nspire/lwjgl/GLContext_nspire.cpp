#ifdef NSPIRE_PLATFORM

#include "lwjgl/GLContext.h"

namespace lwjgl
{
namespace GLContext
{
namespace detail
{
static GLCapabilities &nspireCaps()
{
	static GLCapabilities caps;
	return caps;
}
} // namespace detail

// nGL is a software rasteriser, not a GL context: no extensions, so the
// desktop-only occlusion-query and VBO paths keyed off them stay disabled.
void setRequestedSamples(int) {}
int getRequestedSamples() { return 0; }
void instantiate() {}

const detail::GLCapabilities &getCapabilities()
{
	return detail::nspireCaps();
}

} // namespace GLContext
} // namespace lwjgl

#endif // NSPIRE_PLATFORM
