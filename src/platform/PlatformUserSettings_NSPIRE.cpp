#include "platform/PlatformUserSettings.h"

// No analogue sticks, alternative pad layouts or interlaced output to configure.
namespace PlatformUserSettings
{
void setControllerDeadzone(float) {}
void setAlternativeControls(bool) {}
void setDisplayDeflicker(bool) {}
}
