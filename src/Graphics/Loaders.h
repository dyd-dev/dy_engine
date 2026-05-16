#pragma once
#include "RenderTypes.h"

namespace dy::Graphics
{
	//Image LoadImageFromFile(const const char *filepath);
	bool LoadFromOBJ(const char* path, MeshData& outData);
}
