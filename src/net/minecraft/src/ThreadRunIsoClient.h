#pragma once

#include <thread>
#include "platform/StdThread.h"

class CanvasIsomPreview;

// net.minecraft.src.ThreadRunIsoClient
class ThreadRunIsoClient
{
public:
	ThreadRunIsoClient(CanvasIsomPreview *canvasisompreview);
	~ThreadRunIsoClient();

	void start();
	void run();

private:
	CanvasIsomPreview *isoCanvas;
	PlatformStdThread worker;
};
