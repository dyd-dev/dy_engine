#include "Mesh.h"
#include <cmath>
#include <filesystem>
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
	namespace
	{
		struct ObjMaterialRecord
		{
			ObjMaterialInfo info;
		};

		template <typename T>
		bool IsValidIndex(int index, const std::vector<T>& values)
		{
			return index >= 0 && static_cast<size_t>(index) < values.size();
		}

		dy::Math::float3 Add(const dy::Math::float3& a, const dy::Math::float3& b)
		{
			return dy::Math::float3(a.x + b.x, a.y + b.y, a.z + b.z);
		}

		dy::Math::float3 Subtract(const dy::Math::float3& a, const dy::Math::float3& b)
		{
			return dy::Math::float3(a.x - b.x, a.y - b.y, a.z - b.z);
		}

		dy::Math::float3 Scale(const dy::Math::float3& value, float scale)
		{
			return dy::Math::float3(value.x * scale, value.y * scale, value.z * scale);
		}

		float Dot(const dy::Math::float3& a, const dy::Math::float3& b)
		{
			return a.x * b.x + a.y * b.y + a.z * b.z;
		}

		dy::Math::float3 Cross(const dy::Math::float3& a, const dy::Math::float3& b)
		{
			return dy::Math::float3(
				a.y * b.z - a.z * b.y,
				a.z * b.x - a.x * b.z,
				a.x * b.y - a.y * b.x);
		}

		dy::Math::float3 NormalizeOr(const dy::Math::float3& value, const dy::Math::float3& fallback)
		{
			const float lengthSquared = Dot(value, value);
			if (lengthSquared <= 1.0e-8f) return fallback;
			const float invLength = 1.0f / std::sqrt(lengthSquared);
			return Scale(value, invLength);
		}

		dy::Math::float3 BuildFallbackTangent(const dy::Math::float3& normal)
		{
			const dy::Math::float3 up = std::fabs(normal.z) < 0.999f
				? dy::Math::float3(0.0f, 0.0f, 1.0f)
				: dy::Math::float3(0.0f, 1.0f, 0.0f);
			return NormalizeOr(Cross(up, normal), dy::Math::float3(1.0f, 0.0f, 0.0f));
		}

		void CalculateTangents(MeshData& data)
		{
			std::vector<dy::Math::float3> tangents(data.vertices.size(), dy::Math::float3(0.0f, 0.0f, 0.0f));
			std::vector<dy::Math::float3> bitangents(data.vertices.size(), dy::Math::float3(0.0f, 0.0f, 0.0f));

			for (size_t i = 0; i + 2 < data.indices.size(); i += 3)
			{
				const uint32_t i0 = data.indices[i + 0];
				const uint32_t i1 = data.indices[i + 1];
				const uint32_t i2 = data.indices[i + 2];
				if (i0 >= data.vertices.size() || i1 >= data.vertices.size() || i2 >= data.vertices.size()) continue;

				const Vertex& v0 = data.vertices[i0];
				const Vertex& v1 = data.vertices[i1];
				const Vertex& v2 = data.vertices[i2];

				const dy::Math::float3 edge1 = Subtract(v1.position, v0.position);
				const dy::Math::float3 edge2 = Subtract(v2.position, v0.position);
				const float du1 = v1.uv.x - v0.uv.x;
				const float dv1 = v1.uv.y - v0.uv.y;
				const float du2 = v2.uv.x - v0.uv.x;
				const float dv2 = v2.uv.y - v0.uv.y;
				const float determinant = du1 * dv2 - du2 * dv1;
				if (std::fabs(determinant) <= 1.0e-8f) continue;

				const float invDeterminant = 1.0f / determinant;
				const dy::Math::float3 tangent = Scale(Subtract(Scale(edge1, dv2), Scale(edge2, dv1)), invDeterminant);
				const dy::Math::float3 bitangent = Scale(Subtract(Scale(edge2, du1), Scale(edge1, du2)), invDeterminant);

				tangents[i0] = Add(tangents[i0], tangent);
				tangents[i1] = Add(tangents[i1], tangent);
				tangents[i2] = Add(tangents[i2], tangent);
				bitangents[i0] = Add(bitangents[i0], bitangent);
				bitangents[i1] = Add(bitangents[i1], bitangent);
				bitangents[i2] = Add(bitangents[i2], bitangent);
			}

			for (size_t i = 0; i < data.vertices.size(); ++i)
			{
				Vertex& vertex = data.vertices[i];
				const dy::Math::float3 normal = NormalizeOr(vertex.normal, dy::Math::float3(0.0f, 0.0f, 1.0f));
				const dy::Math::float3 rawTangent = tangents[i];
				const dy::Math::float3 orthogonalTangent = Subtract(rawTangent, Scale(normal, Dot(normal, rawTangent)));
				const dy::Math::float3 tangent = NormalizeOr(orthogonalTangent, BuildFallbackTangent(normal));
				const float handedness = Dot(Cross(normal, tangent), bitangents[i]) < 0.0f ? -1.0f : 1.0f;
				vertex.normal = normal;
				vertex.tangent = dy::Math::float4(tangent.x, tangent.y, tangent.z, handedness);
			}
		}

		void LoadMaterialLibrary(
			const std::filesystem::path& objPath,
			const std::string& libraryName,
			std::map<std::string, ObjMaterialRecord>& materials)
		{
			const std::filesystem::path libraryPath = objPath.parent_path() / libraryName;
			std::ifstream file(libraryPath);
			if (!file.is_open()) return;

			std::string currentName;
			std::string line;
			while (std::getline(file, line))
			{
				std::istringstream ss(line);
				std::string token;
				ss >> token;

				if (token == "newmtl")
				{
					ss >> currentName;
					materials[currentName] = {};
				}
				else if (token == "Kd" && !currentName.empty())
				{
					float r = 1.0f;
					float g = 1.0f;
					float b = 1.0f;
					ss >> r >> g >> b;
					ObjMaterialInfo& info = materials[currentName].info;
					info.material.baseColor = dy::Math::float4(r, g, b, info.material.baseColor.w);
					info.hasBaseColor = true;
				}
				else if (token == "map_Kd" && !currentName.empty())
				{
					std::string textureFile;
					ss >> textureFile;
					if (!textureFile.empty())
					{
						ObjMaterialInfo& info = materials[currentName].info;
						info.baseColorTexturePath = (libraryPath.parent_path() / textureFile).string();
						info.hasBaseColorTexture = true;
					}
				}
			}
		}
	}

	struct OBJIndex
	{
		int v, vt, vn;
		std::string material;
		bool operator<(const OBJIndex& other) const
		{
			if (v != other.v) return v < other.v;
			if (vt != other.vt) return vt < other.vt;
			if (vn != other.vn) return vn < other.vn;
			return material < other.material;
		}
	};

	bool Mesh::LoadFromOBJ(const std::string& path, MeshData& outData)
	{
		return LoadFromOBJ(path, outData, nullptr);
	}

	bool Mesh::LoadFromOBJ(
		const std::string& path,
		MeshData& outData,
		ObjMaterialInfo* outMaterial,
		const ObjLoadOptions& options)
	{
		std::ifstream file(path);
		if (!file.is_open()) return false;

		outData = {};
		if (outMaterial != nullptr) {
			*outMaterial = {};
		}

		const std::filesystem::path objPath(path);
		std::vector<dy::Math::float3> positions;
		std::vector<dy::Math::float2> uvs;
		std::vector<dy::Math::float3> normals;
		std::map<OBJIndex, uint32_t> uniqueVertices;
		std::map<std::string, ObjMaterialRecord> materials;
		std::string currentMaterialName;
		dy::Math::float4 currentColor = dy::Math::float4(1.0f, 1.0f, 1.0f, 1.0f);
		bool capturedMaterial = false;

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
			else if (prefix == "mtllib")
			{
				std::string libraryName;
				ss >> libraryName;
				if (!libraryName.empty()) {
					LoadMaterialLibrary(objPath, libraryName, materials);
				}
			}
			else if (prefix == "usemtl")
			{
				std::string materialName;
				ss >> materialName;
				const auto it = materials.find(materialName);
				if (it != materials.end())
				{
					currentMaterialName = materialName;
					currentColor = it->second.info.material.baseColor;
					if (outMaterial != nullptr && !capturedMaterial)
					{
						*outMaterial = it->second.info;
						capturedMaterial = true;
					}
				}
			}
			else if (prefix == "vt")
			{
				dy::Math::float2 vt;
				ss >> vt.x >> vt.y;
				if (options.flipV) {
					vt.y = 1.0f - vt.y;
				}
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
					OBJIndex idx = { vIdx - 1, vtIdx - 1, vnIdx - 1, currentMaterialName };

					if (uniqueVertices.count(idx) == 0)
					{
						uniqueVertices[idx] = static_cast<uint32_t>(outData.vertices.size());
						Vertex v;
						v.position = IsValidIndex(idx.v, positions) ? positions[static_cast<size_t>(idx.v)] : dy::Math::float3{0,0,0};
						v.uv = IsValidIndex(idx.vt, uvs) ? uvs[static_cast<size_t>(idx.vt)] : dy::Math::float2{0,0};
						v.normal = IsValidIndex(idx.vn, normals) ? normals[static_cast<size_t>(idx.vn)] : dy::Math::float3{0,0,1};
						v.color = currentColor;
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
		if (outMaterial != nullptr && !capturedMaterial && !materials.empty()) {
			*outMaterial = materials.begin()->second.info;
		}
		CalculateTangents(outData);
		return true;
	}

	dy::Mesh ToRenderMesh(const MeshData& meshData)
	{
		dy::Mesh mesh = {};
		mesh.vertices.reserve(meshData.vertices.size());
		mesh.indices = meshData.indices;

		for (const Vertex& source : meshData.vertices)
		{
			dy::Vertex vertex = {};
			vertex.px = source.position.x;
			vertex.py = source.position.y;
			vertex.pz = source.position.z;
			vertex.u = source.uv.x;
			vertex.v = source.uv.y;
			vertex.nx = source.normal.x;
			vertex.ny = source.normal.y;
			vertex.nz = source.normal.z;
			vertex.tx = source.tangent.x;
			vertex.ty = source.tangent.y;
			vertex.tz = source.tangent.z;
			vertex.tw = source.tangent.w;
			mesh.vertices.push_back(vertex);
		}

		return mesh;
	}
}
