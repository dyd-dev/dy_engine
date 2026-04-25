#include "Mesh.h"
#include <fstream>
#include <sstream>
#include <map>

/*
===========================================================================
[TinyObjLoader Usage Guide]
If you need to load more complex OBJ files (materials, multi-shapes, etc.)
in the future, you can replace this custom parser with tinyobjloader.

How to use:
1. Uncomment the following lines:
   #define TINYOBJLOADER_IMPLEMENTATION
   #include <tiny_obj_loader.h>

2. Basic implementation example:
   tinyobj::attrib_t attrib;
   std::vector<tinyobj::shape_t> shapes;
   std::vector<tinyobj::material_t> materials;
   std::string warn, err;

   bool ret = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, path.c_str());
   if (!ret) return false;

   // Loop over shapes and faces to extract vertices/indices...
===========================================================================
*/

namespace dy::Graphics
{
	struct OBJIndex
	{
		int v, vt, vn;
		bool operator<(const OBJIndex& other) const
		{
			if (v != other.v) return v < other.v;
			if (vt != other.vt) return vt < other.vt;
			return vn < other.vn;
		}
	};

	bool Mesh::LoadFromOBJ(const std::string& path, MeshData& outData)
	{
		std::ifstream file(path);
		if (!file.is_open()) return false;

		std::vector<dy::Math::float3> positions;
		std::vector<dy::Math::float2> uvs;
		std::vector<dy::Math::float3> normals;
		std::map<OBJIndex, uint32_t> uniqueVertices;

		std::string line;
		while (std::getline(file, line))
		{
			std::stringstream ss(line);
			std::string prefix;
			ss >> prefix;

			if (prefix == "v")
			{
				dy::Math::float3 v;
				ss >> v.x >> v.y >> v.z;
				positions.push_back(v);
			}
			else if (prefix == "vt")
			{
				dy::Math::float2 vt;
				ss >> vt.x >> vt.y;
				uvs.push_back(vt);
			}
			else if (prefix == "vn")
			{
				dy::Math::float3 vn;
				ss >> vn.x >> vn.y >> vn.z;
				normals.push_back(vn);
			}
			else if (prefix == "f")
			{
				std::string vertexStr;
				std::vector<uint32_t> faceIndices;
				while (ss >> vertexStr)
				{
					int vIdx = 0, vtIdx = 0, vnIdx = 0;
					// Replace / with space for easier parsing
					for (char& c : vertexStr) if (c == '/') c = ' ';
					std::stringstream vss(vertexStr);
					
					vss >> vIdx;
					if (vertexStr.find("  ") != std::string::npos) { // v//vn case
						vss >> vnIdx;
					} else {
						vss >> vtIdx >> vnIdx;
					}

					// Convert to 0-based index
					OBJIndex idx = { vIdx - 1, vtIdx - 1, vnIdx - 1 };

					if (uniqueVertices.count(idx) == 0)
					{
						uniqueVertices[idx] = static_cast<uint32_t>(outData.vertices.size());
						Vertex v;
						v.position = (idx.v >= 0 && idx.v < positions.size()) ? positions[idx.v] : dy::Math::float3{0,0,0};
						v.uv = (idx.vt >= 0 && idx.vt < uvs.size()) ? uvs[idx.vt] : dy::Math::float2{0,0};
						v.normal = (idx.vn >= 0 && idx.vn < normals.size()) ? normals[idx.vn] : dy::Math::float3{0,0,1};
						outData.vertices.push_back(v);
					}
					faceIndices.push_back(uniqueVertices[idx]);
				}

				// Triangulate: Simple fan triangulation for convex polygons
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
}
