#include "Platform/Window.h"
#include <iostream>
#include <string.h>

using namespace dy::Platform;

int main()
{
	Window window(800,600);
	while(window.IsRunning())
	{
		window.PollEvents();
	}
	return 0;
}