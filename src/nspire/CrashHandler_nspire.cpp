// CrashHandler_nspire.cpp -- TI-Nspire implementation of CrashHandler::Crash().
//
// The message goes to the log file next to the program and into an OS dialog,
// after which the program exits back to the document browser.
#ifdef NSPIRE_PLATFORM

#include "pc/CrashHandler.h"
#include "nspire/NspireSystem.h"

namespace CrashHandler
{

void Crash(const std::string &message, const std::string &stackTrace)
{
	if (!stackTrace.empty())
		NspireSystem::log("%s\n", stackTrace.c_str());
	NspireSystem::fatal(message);
}

} // namespace CrashHandler

#endif // NSPIRE_PLATFORM
