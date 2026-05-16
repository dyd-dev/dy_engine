#pragma once

#include "Graphics/MaterialTexturePaths.h"
#include "Graphics/Mesh.h"

#include <string>

namespace dy::Graphics
{
	class FBXLoader
	{
	public:
		static bool Load(const std::string& filepath, MeshData& outData, std::string* outTexturePath = nullptr);
		static bool Load(const std::string& filepath, MeshData& outData, MaterialTexturePaths* outTexturePaths);
		static bool Load(const std::string& filepath, MeshData& outData, MaterialImportData* outMaterialData);
	};
}
