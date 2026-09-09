#pragma once

namespace dy::RHI
{
	class Buffer;
	class Texture;
	class Shader;
	class Pipeline;
	class ResourceSet;

	using BufferHandle = Buffer*;
	using TextureHandle = Texture*;
	using ShaderHandle = Shader*;
	using PipelineHandle = Pipeline*;
	using ResourceSetHandle = ResourceSet*;
}
