#include "Graphics/OBJLoader.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace dy::Graphics
{
	namespace
	{
		std::string TrimLeft(std::string value)
		{
			size_t first = 0;
			while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) ++first;
			return value.substr(first);
		}

		std::string ToLower(std::string value)
		{
			for (char& c : value)
			{
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return value;
		}

		std::string ReadTextureFileToken(std::istringstream& ss)
		{
			std::string token;
			std::string textureFile;
			while (ss >> token)
			{
				if (!token.empty() && token[0] == '#') break;
				textureFile = token;
			}
			return textureFile;
		}

		void AssignTexturePath(const std::filesystem::path& objPath, const std::string& textureFile, std::string& outTexturePath)
		{
			if (textureFile.empty() || !outTexturePath.empty()) return;

			std::filesystem::path texturePath(textureFile);
			if (texturePath.is_relative())
			{
				texturePath = objPath.parent_path() / texturePath;
			}

			outTexturePath = texturePath.lexically_normal().string();
		}

		bool ReadFloat(std::istringstream& ss, float& outValue)
		{
			return static_cast<bool>(ss >> outValue);
		}

		bool ReadFloat3(std::istringstream& ss, float& x, float& y, float& z)
		{
			return static_cast<bool>(ss >> x >> y >> z);
		}

		void ReadMaterialData(const std::filesystem::path& objPath, const std::string& mtllib, MaterialImportData* outMaterialData)
		{
			if (!outMaterialData) return;

			const std::filesystem::path mtlPath = objPath.parent_path() / mtllib;
			std::ifstream mtlFile(mtlPath);
			if (!mtlFile.is_open()) return;

			std::string line;
			while (std::getline(mtlFile, line))
			{
				std::istringstream ss(line);
				std::string type;
				ss >> type;
				type = ToLower(type);

				if (type == "map_kd")
				{
					const std::string textureFile = TrimLeft(ReadTextureFileToken(ss));
					AssignTexturePath(objPath, textureFile, outMaterialData->texturePaths.baseColorTexture);
				}
				else if (type == "map_mr" || type == "map_orm" || type == "map_rma" || type == "map_metallicroughness")
				{
					const std::string textureFile = TrimLeft(ReadTextureFileToken(ss));
					AssignTexturePath(objPath, textureFile, outMaterialData->texturePaths.metallicRoughnessTexture);
				}
				else if (type == "map_bump" || type == "bump" || type == "norm" || type == "map_normal")
				{
					const std::string textureFile = TrimLeft(ReadTextureFileToken(ss));
					AssignTexturePath(objPath, textureFile, outMaterialData->texturePaths.normalTexture);
				}
				else if (type == "map_ka" || type == "map_ao" || type == "ao")
				{
					const std::string textureFile = TrimLeft(ReadTextureFileToken(ss));
					AssignTexturePath(objPath, textureFile, outMaterialData->texturePaths.occlusionTexture);
				}
				else if (type == "map_ke")
				{
					const std::string textureFile = TrimLeft(ReadTextureFileToken(ss));
					AssignTexturePath(objPath, textureFile, outMaterialData->texturePaths.emissiveTexture);
				}
				else if (type == "kd")
				{
					float r = 1.0f;
					float g = 1.0f;
					float b = 1.0f;
					if (ReadFloat3(ss, r, g, b))
					{
						outMaterialData->material.baseColor = dy::Math::float4(r, g, b, outMaterialData->material.baseColor.w);
						outMaterialData->hasBaseColor = true;
					}
				}
				else if (type == "d")
				{
					float alpha = 1.0f;
					if (ReadFloat(ss, alpha))
					{
						outMaterialData->material.baseColor.w = std::clamp(alpha, 0.0f, 1.0f);
						outMaterialData->hasBaseColor = true;
					}
				}
				else if (type == "tr")
				{
					float transparency = 0.0f;
					if (ReadFloat(ss, transparency))
					{
						outMaterialData->material.baseColor.w = std::clamp(1.0f - transparency, 0.0f, 1.0f);
						outMaterialData->hasBaseColor = true;
					}
				}
				else if (type == "ke")
				{
					float r = 0.0f;
					float g = 0.0f;
					float b = 0.0f;
					if (ReadFloat3(ss, r, g, b))
					{
						outMaterialData->material.emissiveColor = dy::Math::float3(r, g, b);
						outMaterialData->hasEmissiveColor = true;
					}
				}
				else if (type == "pm")
				{
					float metallic = 0.0f;
					if (ReadFloat(ss, metallic))
					{
						outMaterialData->material.metallicFactor = std::clamp(metallic, 0.0f, 1.0f);
						outMaterialData->hasMetallicFactor = true;
					}
				}
				else if (type == "pr")
				{
					float roughness = 0.5f;
					if (ReadFloat(ss, roughness))
					{
						outMaterialData->material.roughnessFactor = std::clamp(roughness, 0.04f, 1.0f);
						outMaterialData->hasRoughnessFactor = true;
					}
				}
				else if (type == "ns" && !outMaterialData->hasRoughnessFactor)
				{
					float shininess = 0.0f;
					if (ReadFloat(ss, shininess))
					{
						const float roughness = std::sqrt(2.0f / (std::max(shininess, 0.0f) + 2.0f));
						outMaterialData->material.roughnessFactor = std::clamp(roughness, 0.04f, 1.0f);
						outMaterialData->hasRoughnessFactor = true;
					}
				}
			}
		}

		void ReadOBJMaterialData(const std::string& filepath, MaterialImportData* outMaterialData)
		{
			if (!outMaterialData) return;
			*outMaterialData = {};

			std::ifstream file(filepath);
			if (!file.is_open()) return;

			const std::filesystem::path objPath(filepath);
			std::string line;
			while (std::getline(file, line))
			{
				std::istringstream ss(line);
				std::string type;
				ss >> type;
				if (type != "mtllib") continue;

				std::string mtllib;
				ss >> mtllib;
				if (!mtllib.empty())
				{
					ReadMaterialData(objPath, mtllib, outMaterialData);
					if (outMaterialData->HasAny()) return;
				}
			}
		}
	}

	bool OBJLoader::Load(const std::string& filepath, MeshData& outData, std::string* outTexturePath)
	{
		MaterialTexturePaths texturePaths;
		const bool loaded = Load(filepath, outData, &texturePaths);
		if (outTexturePath)
		{
			*outTexturePath = texturePaths.baseColorTexture;
		}
		return loaded;
	}

	bool OBJLoader::Load(const std::string& filepath, MeshData& outData, MaterialTexturePaths* outTexturePaths)
	{
		MaterialImportData materialData;
		const bool loaded = Load(filepath, outData, &materialData);
		if (outTexturePaths)
		{
			*outTexturePaths = materialData.texturePaths;
		}
		return loaded;
	}

	bool OBJLoader::Load(const std::string& filepath, MeshData& outData, MaterialImportData* outMaterialData)
	{
		outData.vertices.clear();
		outData.indices.clear();
		ReadOBJMaterialData(filepath, outMaterialData);
		return Mesh::LoadFromOBJ(filepath, outData);
	}
}
