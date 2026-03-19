#include "Core/App/Window.h"
#include <iostream>
#include <string.h>

int main()
{
	Window window(800,600);
	while(!window.ShouldClose())
	{
		window.PollEvents();
	}
	return 0;
}