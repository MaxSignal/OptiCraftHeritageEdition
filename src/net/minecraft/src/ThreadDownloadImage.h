#pragma once

#include <string>
#include <thread>
#include "platform/StdThread.h"

class ImageBuffer;
class ThreadDownloadImageData;

// net.minecraft.src.ThreadDownloadImage
class ThreadDownloadImage
{
public:
	ThreadDownloadImage(ThreadDownloadImageData *threaddownloadimagedata, const std::string &s, ImageBuffer *imagebuffer);
	~ThreadDownloadImage();

	void start();
	void run();

private:
	std::string location;
	ImageBuffer *buffer;
	ThreadDownloadImageData *imageData;
	PlatformStdThread worker;
};
