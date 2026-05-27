#pragma once
#include <string>

#include "Mesh.h"

namespace dy::Graphics
{
	bool LoadFromOBJ(const char* path, MeshData& outData);
	bool LoadFromGLTF(const char* filepath, MeshData& outData);
	bool LoadFromGLTF(const char* filepath, MeshData& outData, std::string* outTexturePath);
	bool LoadFromFBX(const char* filepath, MeshData& outData);
	bool LoadFromFBX(const char* filepath, MeshData& outData, std::string* outTexturePath);
}
