#pragma once

#include <string>

#include "Graphics/Mesh.h"

namespace dy::Graphics
{
	class FBXLoader
	{
	public:
		static bool Load(const std::string& filepath, MeshData& outData, std::string* outTexturePath = nullptr);
	};
}
