#include "Graphics/ProfilerHud.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Graphics/RendererShaderLayout.h"
#include "RHI/IBuffer.h"
#include "RHI/ICommandList.h"
#include "RHI/IDevice.h"
#include "RHI/IPipelineState.h"
#include "RHI/ITexture.h"

namespace dy::Graphics
{
namespace
{
	namespace Layout = RendererShaderLayout;

	enum class Palette : uint32_t
	{
		Background,
		Grid,
		White,
		Cyan,
		Green,
		Orange,
		Red,
		Count
	};

	const std::array<Math::float4, static_cast<size_t>(Palette::Count)> kColors = {
		Math::float4(0.015f, 0.020f, 0.030f, 0.84f),
		Math::float4(0.10f, 0.13f, 0.18f, 0.72f),
		Math::float4(3.50f, 3.50f, 3.50f, 1.00f),
		Math::float4(0.10f, 1.80f, 3.00f, 1.00f),
		Math::float4(0.15f, 2.60f, 0.75f, 1.00f),
		Math::float4(3.00f, 1.10f, 0.12f, 1.00f),
		Math::float4(3.00f, 0.20f, 0.18f, 0.90f),
	};

	struct Bucket
	{
		std::vector<ProfilerHud::HudVertex> vertices;
		std::vector<uint32_t> indices;
	};

	struct Canvas
	{
		uint32_t width = 1;
		uint32_t height = 1;
		bool clipYFlip = false;
		std::array<Bucket, static_cast<size_t>(Palette::Count)> buckets;

		[[nodiscard]] float ClipX(float x) const
		{
			return -1.0f + 2.0f * x / static_cast<float>(std::max(width, 1u));
		}

		[[nodiscard]] float ClipY(float y) const
		{
			const float normal = 1.0f - 2.0f * y / static_cast<float>(std::max(height, 1u));
			return clipYFlip ? -normal : normal;
		}

		void Quad(Palette palette, float x0, float y0, float x1, float y1)
		{
			Bucket& bucket = buckets[static_cast<size_t>(palette)];
			const uint32_t first = static_cast<uint32_t>(bucket.vertices.size());
			const float left = ClipX(x0);
			const float right = ClipX(x1);
			const float top = ClipY(y0);
			const float bottom = ClipY(y1);
			bucket.vertices.push_back({ left, top, 0.0f });
			bucket.vertices.push_back({ right, top, 0.0f });
			bucket.vertices.push_back({ right, bottom, 0.0f });
			bucket.vertices.push_back({ left, bottom, 0.0f });
			// D3D12 uses CCW front faces. Vulkan and Metal currently disable culling.
			bucket.indices.insert(bucket.indices.end(), {
				first, first + 2u, first + 1u,
				first, first + 3u, first + 2u
			});
		}

		void Line(Palette palette, float x0, float y0, float x1, float y1, float thickness)
		{
			const float dx = x1 - x0;
			const float dy = y1 - y0;
			const float length = std::sqrt(dx * dx + dy * dy);
			if(length < 0.001f) return;
			const float px = -dy / length * thickness * 0.5f;
			const float py = dx / length * thickness * 0.5f;
			Bucket& bucket = buckets[static_cast<size_t>(palette)];
			const uint32_t first = static_cast<uint32_t>(bucket.vertices.size());
			bucket.vertices.push_back({ ClipX(x0 + px), ClipY(y0 + py), 0.0f });
			bucket.vertices.push_back({ ClipX(x1 + px), ClipY(y1 + py), 0.0f });
			bucket.vertices.push_back({ ClipX(x1 - px), ClipY(y1 - py), 0.0f });
			bucket.vertices.push_back({ ClipX(x0 - px), ClipY(y0 - py), 0.0f });
			bucket.indices.insert(bucket.indices.end(), {
				first, first + 2u, first + 1u,
				first, first + 3u, first + 2u
			});
		}
	};

	[[nodiscard]] std::array<uint8_t, 5> Glyph(char character)
	{
		if(character >= 'a' && character <= 'z') character = static_cast<char>(character - 'a' + 'A');
		switch(character)
		{
		case 'A': return { 0x7e, 0x11, 0x11, 0x11, 0x7e };
		case 'B': return { 0x7f, 0x49, 0x49, 0x49, 0x36 };
		case 'C': return { 0x3e, 0x41, 0x41, 0x41, 0x22 };
		case 'D': return { 0x7f, 0x41, 0x41, 0x22, 0x1c };
		case 'E': return { 0x7f, 0x49, 0x49, 0x49, 0x41 };
		case 'F': return { 0x7f, 0x09, 0x09, 0x09, 0x01 };
		case 'G': return { 0x3e, 0x41, 0x49, 0x49, 0x7a };
		case 'H': return { 0x7f, 0x08, 0x08, 0x08, 0x7f };
		case 'I': return { 0x00, 0x41, 0x7f, 0x41, 0x00 };
		case 'J': return { 0x20, 0x40, 0x41, 0x3f, 0x01 };
		case 'K': return { 0x7f, 0x08, 0x14, 0x22, 0x41 };
		case 'L': return { 0x7f, 0x40, 0x40, 0x40, 0x40 };
		case 'M': return { 0x7f, 0x02, 0x0c, 0x02, 0x7f };
		case 'N': return { 0x7f, 0x04, 0x08, 0x10, 0x7f };
		case 'O': return { 0x3e, 0x41, 0x41, 0x41, 0x3e };
		case 'P': return { 0x7f, 0x09, 0x09, 0x09, 0x06 };
		case 'Q': return { 0x3e, 0x41, 0x51, 0x21, 0x5e };
		case 'R': return { 0x7f, 0x09, 0x19, 0x29, 0x46 };
		case 'S': return { 0x46, 0x49, 0x49, 0x49, 0x31 };
		case 'T': return { 0x01, 0x01, 0x7f, 0x01, 0x01 };
		case 'U': return { 0x3f, 0x40, 0x40, 0x40, 0x3f };
		case 'V': return { 0x1f, 0x20, 0x40, 0x20, 0x1f };
		case 'W': return { 0x3f, 0x40, 0x38, 0x40, 0x3f };
		case 'X': return { 0x63, 0x14, 0x08, 0x14, 0x63 };
		case 'Y': return { 0x07, 0x08, 0x70, 0x08, 0x07 };
		case 'Z': return { 0x61, 0x51, 0x49, 0x45, 0x43 };
		case '0': return { 0x3e, 0x51, 0x49, 0x45, 0x3e };
		case '1': return { 0x00, 0x42, 0x7f, 0x40, 0x00 };
		case '2': return { 0x42, 0x61, 0x51, 0x49, 0x46 };
		case '3': return { 0x21, 0x41, 0x45, 0x4b, 0x31 };
		case '4': return { 0x18, 0x14, 0x12, 0x7f, 0x10 };
		case '5': return { 0x27, 0x45, 0x45, 0x45, 0x39 };
		case '6': return { 0x3c, 0x4a, 0x49, 0x49, 0x30 };
		case '7': return { 0x01, 0x71, 0x09, 0x05, 0x03 };
		case '8': return { 0x36, 0x49, 0x49, 0x49, 0x36 };
		case '9': return { 0x06, 0x49, 0x49, 0x29, 0x1e };
		case '.': return { 0x00, 0x60, 0x60, 0x00, 0x00 };
		case ':': return { 0x00, 0x36, 0x36, 0x00, 0x00 };
		case '-': return { 0x08, 0x08, 0x08, 0x08, 0x08 };
		case '/': return { 0x20, 0x10, 0x08, 0x04, 0x02 };
		case '%': return { 0x23, 0x13, 0x08, 0x64, 0x62 };
		default: return { 0, 0, 0, 0, 0 };
		}
	}

	void Text(Canvas& canvas, Palette palette, float x, float y, const std::string& text, float scale = 2.0f)
	{
		float cursor = x;
		for(char character : text)
		{
			if(character == ' ')
			{
				cursor += 6.0f * scale;
				continue;
			}
			const std::array<uint8_t, 5> columns = Glyph(character);
			for(uint32_t column = 0; column < columns.size(); ++column)
			{
				for(uint32_t row = 0; row < 7u; ++row)
				{
					if((columns[column] & (1u << row)) == 0u) continue;
					const float px = cursor + static_cast<float>(column) * scale;
					const float py = y + static_cast<float>(row) * scale;
					canvas.Quad(palette, px, py, px + scale, py + scale);
				}
			}
			cursor += 6.0f * scale;
		}
	}

	[[nodiscard]] std::string ValueLine(const char* label, double value, const char* suffix = "")
	{
		char buffer[96] = {};
		std::snprintf(buffer, sizeof(buffer), "%s  %.2f%s", label, value, suffix);
		return buffer;
	}

	[[nodiscard]] std::string ResourceLine(const char* label, uint64_t live, uint64_t created, uint64_t destroyed)
	{
		char buffer[96] = {};
		std::snprintf(
			buffer,
			sizeof(buffer),
			"%s L %llu C %llu D %llu",
			label,
			static_cast<unsigned long long>(live),
			static_cast<unsigned long long>(created),
			static_cast<unsigned long long>(destroyed));
		return buffer;
	}

	void Upload(RHI::IBuffer* buffer, const void* source, uint32_t size)
	{
		if(buffer == nullptr || source == nullptr || size == 0u) return;
		if(void* mapped = buffer->Map(0))
		{
			std::memcpy(mapped, source, size);
			buffer->Unmap();
		}
	}
}

static_assert(sizeof(ProfilerHud::HudVertex) == sizeof(float) * RendererShaderLayout::kRendererVertexFloatCount,
	"Profiler HUD vertex layout must match the renderer shader contract.");

void ProfilerHud::Initialize(RHI::IDevice* device, bool startsExpanded)
{
	m_expanded = startsExpanded;
	const uint32_t frameCount = device != nullptr ? std::max(device->GetDesc().maxFramesInFlight, 1u) : 1u;
	m_frames.resize(frameCount);
	m_frameHistory.assign(kHistorySize, 0.0f);
	m_cpuHistory.assign(kHistorySize, 0.0f);
	m_gpuHistory.assign(kHistorySize, 0.0f);
}

void ProfilerHud::Shutdown(RHI::IDevice* device)
{
	if(device != nullptr)
	{
		for(FrameResources& frame : m_frames)
		{
			if(frame.vertexBuffer != nullptr) device->DestroyBuffer(frame.vertexBuffer);
			if(frame.indexBuffer != nullptr) device->DestroyBuffer(frame.indexBuffer);
			if(frame.lightingBuffer != nullptr) device->DestroyBuffer(frame.lightingBuffer);
			frame = {};
		}
	}
	m_frames.clear();
	m_vertices.clear();
	m_indices.clear();
	m_batches.clear();
	m_historyCursor = 0;
	m_historyCount = 0;
}

void ProfilerHud::PushHistory(const ProfilerHudMetrics& metrics)
{
	if(m_frameHistory.empty()) return;
	m_frameHistory[m_historyCursor] = static_cast<float>(metrics.frameMilliseconds);
	m_cpuHistory[m_historyCursor] = static_cast<float>(metrics.cpuRenderMilliseconds);
	m_gpuHistory[m_historyCursor] = metrics.hasGpuMain ? static_cast<float>(metrics.gpuMainMilliseconds) : 0.0f;
	m_historyCursor = (m_historyCursor + 1u) % kHistorySize;
	m_historyCount = std::min(m_historyCount + 1u, kHistorySize);
	m_hasGpuMain = metrics.hasGpuMain;
}

void ProfilerHud::PrepareFrame(
	RHI::IDevice* device,
	const ProfilerHudMetrics& metrics,
	uint32_t width,
	uint32_t height,
	bool clipYFlip)
{
	if(device == nullptr || m_frames.empty() || width == 0u || height == 0u) return;
	m_viewportWidth = width;
	m_viewportHeight = height;
	PushHistory(metrics);
	BuildGeometry(width, height, clipYFlip, metrics);
	m_currentFrame = device->GetCurrentFrameIndex() % static_cast<uint32_t>(m_frames.size());
	EnsureAndUpload(device, m_frames[m_currentFrame]);
}

void ProfilerHud::BuildGeometry(uint32_t width, uint32_t height, bool clipYFlip, const ProfilerHudMetrics& metrics)
{
	Canvas canvas = {};
	canvas.width = width;
	canvas.height = height;
	canvas.clipYFlip = clipYFlip;
	const float x = 14.0f;
	const float y = 14.0f;

	if(!m_expanded)
	{
		canvas.Quad(Palette::Background, x, y, x + 282.0f, y + 114.0f);
		Text(canvas, Palette::Cyan, x + 12.0f, y + 10.0f, "DY PROFILER");
		Text(canvas, Palette::White, x + 168.0f, y + 10.0f, "F11");
		Text(canvas, Palette::White, x + 12.0f, y + 32.0f, ValueLine("FPS", metrics.fps));
		Text(canvas, Palette::Green, x + 12.0f, y + 52.0f, ValueLine("CPU", metrics.cpuRenderMilliseconds, " MS"));
		Text(canvas, Palette::Orange, x + 12.0f, y + 72.0f,
			metrics.hasGpuMain ? ValueLine("GPU", metrics.gpuMainMilliseconds, " MS") : "GPU  N/A");
		char resources[64] = {};
		std::snprintf(
			resources,
			sizeof(resources),
			"RES B %llu T %llu P %llu",
			static_cast<unsigned long long>(metrics.liveBuffers),
			static_cast<unsigned long long>(metrics.liveTextures),
			static_cast<unsigned long long>(metrics.livePipelines));
		Text(canvas, Palette::White, x + 12.0f, y + 92.0f, resources);
	}
	else
	{
		const float panelWidth = std::min(700.0f, static_cast<float>(width) - 28.0f);
		const float panelHeight = std::min(430.0f, static_cast<float>(height) - 28.0f);
		canvas.Quad(Palette::Background, x, y, x + panelWidth, y + panelHeight);
		Text(canvas, Palette::Cyan, x + 14.0f, y + 12.0f, "DY ENGINE PROFILER", 2.0f);
		Text(canvas, Palette::White, x + panelWidth - 190.0f, y + 12.0f, "F11 HIDE", 2.0f);
		Text(canvas, Palette::White, x + 14.0f, y + 38.0f, ValueLine("FPS", metrics.fps));
		Text(canvas, Palette::Cyan, x + 14.0f, y + 60.0f, ValueLine("FRAME", metrics.frameMilliseconds, " MS"));
		Text(canvas, Palette::Green, x + 240.0f, y + 38.0f, ValueLine("CPU RENDER", metrics.cpuRenderMilliseconds, " MS"));
		Text(canvas, Palette::Orange, x + 240.0f, y + 60.0f,
			metrics.hasGpuMain ? ValueLine("GPU MAIN", metrics.gpuMainMilliseconds, " MS") : "GPU MAIN  N/A");
		Text(canvas, Palette::White, x + 14.0f, y + 88.0f,
			ResourceLine("BUFFER", metrics.liveBuffers, metrics.createdBuffers, metrics.destroyedBuffers));
		Text(canvas, Palette::White, x + 14.0f, y + 108.0f,
			ResourceLine("TEXTURE", metrics.liveTextures, metrics.createdTextures, metrics.destroyedTextures));
		Text(canvas, Palette::White, x + 14.0f, y + 128.0f,
			ResourceLine("PIPELINE", metrics.livePipelines, metrics.createdPipelines, metrics.destroyedPipelines));
		Text(canvas, Palette::White, x + 14.0f, y + 154.0f, "FRAME TIME HISTORY");

		const float graphX = x + 16.0f;
		const float graphY = y + 182.0f;
		const float graphW = panelWidth - 32.0f;
		const float graphH = panelHeight - 222.0f;
		canvas.Quad(Palette::Grid, graphX, graphY, graphX + graphW, graphY + graphH);

		float scaleMs = 33.33f;
		for(uint32_t i = 0; i < m_historyCount; ++i)
		{
			const uint32_t index = (m_historyCursor + kHistorySize - m_historyCount + i) % kHistorySize;
			scaleMs = std::max(scaleMs, std::max({ m_frameHistory[index], m_cpuHistory[index], m_gpuHistory[index] }) * 1.15f);
		}
		for(uint32_t line = 1; line < 4; ++line)
		{
			const float gy = graphY + graphH * static_cast<float>(line) / 4.0f;
			canvas.Line(Palette::Background, graphX, gy, graphX + graphW, gy, 1.0f);
		}
		const float budgetY = graphY + graphH - std::min(16.67f / scaleMs, 1.0f) * graphH;
		canvas.Line(Palette::Red, graphX, budgetY, graphX + graphW, budgetY, 1.2f);

		auto drawHistory = [&](const std::vector<float>& values, Palette palette)
		{
			if(m_historyCount < 2u) return;
			float previousX = graphX;
			float previousY = graphY + graphH;
			for(uint32_t i = 0; i < m_historyCount; ++i)
			{
				const uint32_t index = (m_historyCursor + kHistorySize - m_historyCount + i) % kHistorySize;
				const float px = graphX + graphW * static_cast<float>(i) / static_cast<float>(kHistorySize - 1u);
				const float normalized = std::clamp(values[index] / scaleMs, 0.0f, 1.0f);
				const float py = graphY + graphH - normalized * graphH;
				if(i > 0u) canvas.Line(palette, previousX, previousY, px, py, 2.0f);
				previousX = px;
				previousY = py;
			}
		};
		drawHistory(m_frameHistory, Palette::Cyan);
		drawHistory(m_cpuHistory, Palette::Green);
		if(m_hasGpuMain) drawHistory(m_gpuHistory, Palette::Orange);

		Text(canvas, Palette::Cyan, graphX, graphY + graphH + 10.0f, "FRAME");
		Text(canvas, Palette::Green, graphX + 100.0f, graphY + graphH + 10.0f, "CPU");
		Text(canvas, Palette::Orange, graphX + 170.0f, graphY + graphH + 10.0f, "GPU");
		Text(canvas, Palette::Red, graphX + 240.0f, graphY + graphH + 10.0f, "16.67 MS");
		char scaleLabel[48] = {};
		std::snprintf(scaleLabel, sizeof(scaleLabel), "SCALE %.1F MS", scaleMs);
		Text(canvas, Palette::White, graphX + graphW - 170.0f, graphY + graphH + 10.0f, scaleLabel);
	}

	m_vertices.clear();
	m_indices.clear();
	m_batches.clear();
	for(uint32_t palette = 0; palette < static_cast<uint32_t>(Palette::Count); ++palette)
	{
		Bucket& bucket = canvas.buckets[palette];
		if(bucket.indices.empty()) continue;
		const uint32_t vertexBase = static_cast<uint32_t>(m_vertices.size());
		const uint32_t firstIndex = static_cast<uint32_t>(m_indices.size());
		m_vertices.insert(m_vertices.end(), bucket.vertices.begin(), bucket.vertices.end());
		for(uint32_t index : bucket.indices) m_indices.push_back(vertexBase + index);
		m_batches.push_back({ firstIndex, static_cast<uint32_t>(bucket.indices.size()), kColors[palette] });
	}
}

void ProfilerHud::EnsureAndUpload(RHI::IDevice* device, FrameResources& resources)
{
	const uint32_t vertexBytes = static_cast<uint32_t>(m_vertices.size() * sizeof(HudVertex));
	const uint32_t indexBytes = static_cast<uint32_t>(m_indices.size() * sizeof(uint32_t));
	if(vertexBytes == 0u || indexBytes == 0u) return;

	auto ensure = [&](RHI::IBuffer*& buffer, uint32_t& capacity, uint32_t required, uint32_t stride, RHI::BufferUsage usage)
	{
		if(buffer != nullptr && capacity >= required) return;
		if(buffer != nullptr) device->DestroyBuffer(buffer);
		capacity = std::max(required, std::max(capacity * 2u, 4096u));
		buffer = device->CreateBuffer({ capacity, stride, usage });
	};
	ensure(resources.vertexBuffer, resources.vertexCapacityBytes, vertexBytes, sizeof(HudVertex), RHI::BufferUsage::Vertex | RHI::BufferUsage::Storage);
	ensure(resources.indexBuffer, resources.indexCapacityBytes, indexBytes, sizeof(uint32_t), RHI::BufferUsage::Index | RHI::BufferUsage::Storage);
	if(resources.lightingBuffer == nullptr)
	{
		RHI::BufferDesc desc = {};
		desc.size = sizeof(Layout::RendererLightingConstants);
		desc.usage = RHI::BufferUsage::Constant;
		resources.lightingBuffer = device->CreateBuffer(desc);
	}
	// HUD colors have their own SDR conversion, independent of scene exposure/HDR.
	Layout::RendererLightingConstants lighting = {};
	const auto* backBuffer = device->GetBackBuffer();
	lighting.pbrParams = Math::float4(0.04f, 0.0f,
		backBuffer != nullptr && RHI::IsSrgbFormat(backBuffer->GetFormat()) ? 0.0f : 1.0f, 1.0f);
	Upload(resources.lightingBuffer, &lighting, sizeof(lighting));
	Upload(resources.vertexBuffer, m_vertices.data(), vertexBytes);
	Upload(resources.indexBuffer, m_indices.data(), indexBytes);
}

void ProfilerHud::Record(
	RHI::ICommandList* commandList,
	RHI::IPipelineState* pipeline,
	RHI::IBuffer* lightingBuffer,
	RHI::IBuffer* shadowMatrixBuffer) const
{
	if(commandList == nullptr || pipeline == nullptr || m_frames.empty() || m_batches.empty()) return;
	const FrameResources& resources = m_frames[m_currentFrame];
	if(resources.vertexBuffer == nullptr || resources.indexBuffer == nullptr || resources.lightingBuffer == nullptr) return;
	// Optional HUD draws must not make an otherwise valid scene exceed its frame budget.
	if(m_batches.size() > commandList->GetRemainingDrawCapacity()) return;
	lightingBuffer = resources.lightingBuffer;

	RHI::CommandDebugEventScope event(commandList, "ProfilerHUD", { 0.15f, 0.90f, 0.55f, 1.0f });
	commandList->SetViewport({ 0.0f, 0.0f, static_cast<float>(m_viewportWidth), static_cast<float>(m_viewportHeight), 0.0f, 1.0f });
	commandList->SetScissor({ 0, 0, m_viewportWidth, m_viewportHeight });
	commandList->BindGraphicsPipeline(pipeline);
	commandList->BindGlobalDescriptors();
	if(lightingBuffer != nullptr)
	{
		commandList->BindConstantBuffer(Layout::kLightingConstantBinding, lightingBuffer, 0, sizeof(Layout::RendererLightingConstants));
	}
	if(shadowMatrixBuffer != nullptr)
	{
		commandList->BindConstantBuffer(Layout::kShadowMatrixBinding, shadowMatrixBuffer, 0, sizeof(Layout::RendererShadowConstants));
	}
	RHI::GeometryBinding geometry = {};
	geometry.vertexBuffer = resources.vertexBuffer;
	geometry.vertexStride = sizeof(HudVertex);
	geometry.indexBuffer = resources.indexBuffer;
	geometry.indexFormat = RHI::Format::R32_UINT;
	commandList->BindGeometry(geometry);

	for(const Batch& batch : m_batches)
	{
		Layout::DrawConstants draw = {};
		draw.viewProjectionMatrix = Math::float4x4::Identity();
		draw.modelMatrix = Math::float4x4::Identity();
		draw.firstIndex = batch.firstIndex;
		draw.emissiveColor = Math::float4(batch.color.x, batch.color.y, batch.color.z, 0.0f);
		draw.baseColor = Math::float4(0.0f, 0.0f, 0.0f, batch.color.w);
		draw.materialParams = Math::float4(0.0f, 1.0f, 1.0f, 1.0f);
		commandList->SetInlineConstants(sizeof(draw), &draw);
		commandList->DrawIndexedInstanced(batch.indexCount, 1u, batch.firstIndex, 0, 0u);
	}
}
}
