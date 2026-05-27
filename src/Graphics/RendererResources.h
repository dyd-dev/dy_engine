#pragma once
#include <cstdint>
#include <vector>

#include "Material.h"
#include "Mesh.h"

namespace dy::RHI
{
	class IBuffer;
	class ITexture;
}

namespace dy::Graphics
{
	struct GpuMesh
	{
		RHI::IBuffer* vertexBuffer = nullptr;
		RHI::IBuffer* indexBuffer = nullptr;
		uint32_t vertexStride = sizeof(Vertex);
		uint32_t vertexOffset = 0;
		uint32_t indexOffset = 0;
		uint32_t vertexByteSize = 0;
		uint32_t indexByteSize = 0;
		uint32_t vertexCount = 0;
		uint32_t indexCount = 0;
	};

	struct GpuTexture
	{
		RHI::ITexture* texture = nullptr;
		uint32_t width = 0;
		uint32_t height = 0;
	};

	struct GpuMaterial
	{
		MaterialDrawConstants constants = {};
	};

	struct RendererResources
	{
		std::vector<GpuMesh> meshes;
		std::vector<GpuTexture> textures;
		std::vector<GpuMaterial> materials;
	};
}
