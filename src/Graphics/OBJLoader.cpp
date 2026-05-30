#include "OBJLoader.h"

#include "Graphics/Loaders.h"

#include <fstream>
#include <map>
#include <sstream>

namespace dy::Graphics
{
	struct OBJIndex
	{
		int v = 0;
		int vt = 0;
		int vn = 0;

		bool operator<(const OBJIndex& other) const
		{
			if (v != other.v) return v < other.v;
			if (vt != other.vt) return vt < other.vt;
			return vn < other.vn;
		}
	};

	bool LoadFromOBJ(const char* path, MeshData& outData)
	{
		std::ifstream file(path);
		if (!file.is_open()) return false;

		std::vector<Math::float3> positions;
		std::vector<Math::float2> uvs;
		std::vector<Math::float3> normals;
		std::map<OBJIndex, uint32_t> uniqueVertices;

		std::string line;
		while (std::getline(file, line))
		{
			std::stringstream ss(line);
			std::string prefix;
			ss >> prefix;

			if (prefix == "v")
			{
				Math::float3 v;
				ss >> v.x >> v.y >> v.z;
				positions.push_back(v);
			}
			else if (prefix == "vt")
			{
				Math::float2 vt;
				ss >> vt.x >> vt.y;
				uvs.push_back(vt);
			}
			else if (prefix == "vn")
			{
				Math::float3 vn;
				ss >> vn.x >> vn.y >> vn.z;
				normals.push_back(vn);
			}
			else if (prefix == "f")
			{
				std::string vertexStr;
				std::vector<uint32_t> faceIndices;
				while (ss >> vertexStr)
				{
					int vIdx = 0;
					int vtIdx = 0;
					int vnIdx = 0;
					for (char& c : vertexStr) if (c == '/') c = ' ';
					std::stringstream vss(vertexStr);

					vss >> vIdx;
					if (vertexStr.find("  ") != std::string::npos) {
						vss >> vnIdx;
					} else {
						vss >> vtIdx >> vnIdx;
					}

					OBJIndex idx = { vIdx - 1, vtIdx - 1, vnIdx - 1 };
					if (uniqueVertices.count(idx) == 0)
					{
						uniqueVertices[idx] = static_cast<uint32_t>(outData.vertices.size());
						Vertex v;
						v.position = (idx.v >= 0 && idx.v < static_cast<int>(positions.size())) ? positions[idx.v] : Math::float3(0.0f, 0.0f, 0.0f);
						v.uv = (idx.vt >= 0 && idx.vt < static_cast<int>(uvs.size())) ? uvs[idx.vt] : Math::float2(0.0f, 0.0f);
						v.normal = (idx.vn >= 0 && idx.vn < static_cast<int>(normals.size())) ? normals[idx.vn] : Math::float3(0.0f, 0.0f, 1.0f);
						outData.vertices.push_back(v);
					}
					faceIndices.push_back(uniqueVertices[idx]);
				}

				for (size_t i = 1; i < faceIndices.size() - 1; ++i)
				{
					outData.indices.push_back(faceIndices[0]);
					outData.indices.push_back(faceIndices[i]);
					outData.indices.push_back(faceIndices[i + 1]);
				}
			}
		}
		return true;
	}

	bool OBJLoader::Load(const std::string& filepath, MeshData& outData, std::string* outTexturePath)
	{
		(void)outTexturePath;
		return LoadFromOBJ(filepath.c_str(), outData);
	}
}
