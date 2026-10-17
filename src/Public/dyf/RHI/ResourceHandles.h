#pragma once

namespace dyf::RHI
{
	class Buffer;
	class Texture;
	class Shader;
	class Pipeline;
	class ResourceSet;
	class TimestampQuery;

	using BufferHandle = Buffer*;
	using TextureHandle = Texture*;
	using ShaderHandle = Shader*;
	using PipelineHandle = Pipeline*;
	using ResourceSetHandle = ResourceSet*;
	using TimestampQueryHandle = TimestampQuery*;
}
