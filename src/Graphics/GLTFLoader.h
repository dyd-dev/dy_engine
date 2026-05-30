#pragma once

#include <string>

#include "Graphics/Mesh.h"

namespace dy::Graphics
{
	class GLTFLoader
	{
	public:
		static bool Load(const std::string& filepath, MeshData& outData, std::string* outTexturePath = nullptr);
	};
}
