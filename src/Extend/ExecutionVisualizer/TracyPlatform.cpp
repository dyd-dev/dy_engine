#include "TracyPlatform.h"
#include "dyf/Extends/ImGui/ImGui.h"
#include "dyf/RHI.h"
#include "Fonts.hpp"
#include "profiler/TracyEmbed.hpp"
#include "profiler/TracyTexture.hpp"
#include "profiler/TracyMouse.hpp"
#include "profiler/IconsFontAwesome6.h"
#include "public/common/TracyForceInline.hpp"
#include "data/FontNormal.hpp"
#include "data/FontFixed.hpp"
#include "data/FontBold.hpp"
#include "data/FontItalic.hpp"
#include "data/FontBoldItalic.hpp"
#include "data/FontIcons.hpp"
#include "imgui_internal.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <stb_image.h>
#include "zigzag32.hpp"
#include "zigzag16.hpp"
#include "zigzag08.hpp"
#include "zigzag04.hpp"
#include "zigzag02.hpp"
#include "zigzag01.hpp"

FontData g_fonts{};
float FontNormal = 22, FontSmall = 15, FontBig = 31;
ImTextureID zigzagTex = 0;
namespace RHI = dyf::RHI;

// Keep each decompressed font owned by ImGui until its shared atlas is destroyed.
void LoadFonts(float)
{
    auto* atlas = ImGui::GetIO().Fonts;
    static const ImWchar textRanges[] = {0x20, 0x024f, 0x0370, 0x03ff, 0x2000, 0x206f, 0x2190, 0x21ff, 0};
    static const ImWchar iconRanges[] = {ICON_MIN_FA, ICON_MAX_16_FA, 0};
    const auto icons = Unembed(FontIcons);
    const auto add = [&](const std::shared_ptr<EmbedData>& bytes, bool merge, const ImWchar* ranges)
    {
        ImFontConfig config;
        config.MergeMode = merge;
        config.OversampleH = config.OversampleV = 1;
        void* owned = IM_ALLOC(bytes->size());
        if(!owned) throw std::bad_alloc();
        std::memcpy(owned, bytes->data(), bytes->size());
        auto* result = atlas->AddFontFromMemoryTTF(owned, int(bytes->size()), FontNormal, &config, ranges);
        if(!result) throw std::runtime_error("Tracy font load failed");
        return result;
    };
    const auto face = [&](const std::shared_ptr<EmbedData>& bytes)
    {
        auto* result = add(bytes, false, textRanges);
        add(icons, true, iconRanges);
        return result;
    };
    g_fonts.normal = face(Unembed(FontNormal));
    g_fonts.mono = face(Unembed(FontFixed));
    g_fonts.bold = face(Unembed(FontBold));
    g_fonts.italic = face(Unembed(FontItalic));
    g_fonts.boldItalic = face(Unembed(FontBoldItalic));
    atlas->Build();
    const std::vector<ImFont*> faces = {g_fonts.normal, g_fonts.mono, g_fonts.bold, g_fonts.italic, g_fonts.boldItalic};
    for(const float size : {FontSmall, FontBig, FontNormal * 2})
    {
        for(auto* font : faces) font->LegacySize = size;
        ImFontAtlasBuildLegacyPreloadAllGlyphRanges(atlas);
    }
    for(auto* font : faces) { font->LegacySize = FontNormal; font->Flags |= ImFontFlags_LockBakedSizes; }
}

namespace viewer
{
TracyImageSize CheckedTracyImageSize(int width, int height)
{
    if(width <= 0 || height <= 0 || uint64_t(width) * uint64_t(height) * 4 > UINT32_MAX)
        throw std::invalid_argument("Invalid Tracy image dimensions");
    const auto row = uint64_t(width) * 4;
    return {uint32_t(row), uint32_t(row * uint64_t(height))};
}
struct TracyPlatform::Impl
{
    dyf::Gui& gui;
    RHI::IDevice& device;
    const std::thread::id owner = std::this_thread::get_id();
    std::unordered_map<ImTextureID, RHI::TextureHandle> active;
    std::vector<std::pair<ImTextureID, RHI::TextureHandle>> retired;
    std::unordered_set<ImGuiID> windows;
    std::unordered_map<ImGuiID, int> beginCounts;
    bool inputAllowed = false;
    bool mouseOwned[ImGuiMouseButton_COUNT]{};
    static Impl* current;
    void CheckThread() const
    {
        if(std::this_thread::get_id() != owner) throw std::logic_error("Tracy graphics must run on the Gui thread");
    }
    void Retire(ImTextureID id)
    {
        CheckThread();
        const auto found = active.find(id);
        if(found == active.end()) return;
        retired.emplace_back(id, found->second);
        active.erase(found);
    }
    void Collect()
    {
        CheckThread();
        if(retired.empty()) return;
        if(!device.WaitIdle()) throw std::runtime_error("Tracy texture GPU wait failed");
        for(const auto& entry : retired) { gui.UnregisterTexture(entry.first); device.DestroyTexture(entry.second); }
        retired.clear();
    }
    void Upload(ImTextureID& id, void** pixels, const int* widths, const int* heights, size_t mips)
    {
        CheckThread();
        if(id && active.find(id) == active.end()) throw std::invalid_argument("Tracy texture ID is no longer alive");
        if(!mips || mips > 32 || !pixels || !widths || !heights || widths[0] <= 0 || heights[0] <= 0)
            throw std::invalid_argument("Invalid Tracy texture dimensions");
        for(size_t i=0; i<mips; ++i)
        {
            if(!pixels[i] || widths[i] != std::max(1, widths[0] >> i) || heights[i] != std::max(1, heights[0] >> i))
                throw std::invalid_argument("Invalid Tracy texture mip");
            (void)CheckedTracyImageSize(widths[i], heights[i]);
        }
        auto* texture = device.CreateTexture({uint32_t(widths[0]), uint32_t(heights[0]), 1, uint32_t(mips),
            RHI::Format::R8G8B8A8_UNORM_SRGB, RHI::TextureUsage::ShaderResource});
        if(!texture) throw std::runtime_error("Tracy texture creation failed");
        auto* commands = device.AcquireCommandList();
        bool uploaded = commands != nullptr;
        if(commands)
        {
            RHI::ResourceBarrierDesc barrier{nullptr, texture, RHI::ResourceState::Undefined, RHI::ResourceState::CopyDestination, {}};
            commands->ResourceBarrier(&barrier, 1);
            for(size_t i=0; i<mips && uploaded; ++i)
            {
                const auto size = CheckedTracyImageSize(widths[i], heights[i]);
                uploaded = device.UpdateTexture(*commands, texture, uint32_t(i), 0, pixels[i], size.totalBytes,
                    size.rowBytes, size.totalBytes);
            }
            barrier.before = RHI::ResourceState::CopyDestination; barrier.after = RHI::ResourceState::ShaderResource;
            commands->ResourceBarrier(&barrier, 1);
            uploaded = uploaded && commands->Close() && device.Submit(&commands, 1);
            device.DestroyCommandList(commands);
        }
        if(!uploaded)
        {
            if(device.WaitIdle()) device.DestroyTexture(texture);
            throw std::runtime_error("Tracy texture upload failed");
        }
        const auto replacement = gui.RegisterTexture(texture);
        if(!replacement)
        {
            if(device.WaitIdle()) device.DestroyTexture(texture);
            throw std::runtime_error("Tracy texture registration failed");
        }
        active.emplace(replacement, texture);
        if(id) Retire(id);
        id = replacement;
    }
};
TracyPlatform::Impl* TracyPlatform::Impl::current = nullptr;

TracyPlatform::TracyPlatform(dyf::Gui& gui, RHI::IDevice& device) : impl(std::make_unique<Impl>(Impl{gui, device}))
{
    if(Impl::current) throw std::logic_error("Only one embedded Tracy graphics adapter is supported");
    Impl::current = impl.get();
}
TracyPlatform::~TracyPlatform()
{
    for(const auto& entry : impl->active) impl->retired.push_back(entry);
    impl->active.clear();
    try { impl->Collect(); } catch(...) { /* A lost device owns the remaining native resources. */ }
    zigzagTex = 0;
    Impl::current = nullptr;
}
void TracyPlatform::Tick() { impl->Collect(); }
void TracyPlatform::MouseFrame(bool visible)
{
    const auto* hovered = ImGui::GetCurrentContext()->HoveredWindow;
    const bool enabled = visible && hovered && (impl->windows.count(hovered->ID) || hovered->ID == ImHashStr("Tracy###Profiler"));
    tracy::MouseFrame(enabled);
    const auto& io = ImGui::GetIO();
    for(int button=0; button<ImGuiMouseButton_COUNT; ++button)
    {
        if(!visible || io.MouseClicked[button]) impl->mouseOwned[button] = enabled;
        if(!io.MouseDown[button] && !io.MouseReleased[button]) impl->mouseOwned[button] = false;
        if(!impl->mouseOwned[button]) tracy::ConsumeMouseEvents(button);
    }
}
void TracyPlatform::BeginDraw()
{
    auto* ctx = ImGui::GetCurrentContext();
    impl->inputAllowed = ctx->NavWindow && impl->windows.count(ctx->NavWindow->ID) != 0;
    impl->beginCounts.clear();
    for(auto* window : ctx->Windows) impl->beginCounts.emplace(window->ID, window->BeginCount);
}
void TracyPlatform::EndDraw()
{
    for(auto* window : ImGui::GetCurrentContext()->Windows)
    {
        const auto found = impl->beginCounts.find(window->ID);
        if(window->LastFrameActive == ImGui::GetFrameCount() && (found == impl->beginCounts.end() || found->second != window->BeginCount))
            impl->windows.insert(window->ID);
    }
}
// The only process-global routing belongs to Tracy's original texture callbacks.
TracyPlatform::Impl& TracyGraphics()
{
    auto* result = TracyPlatform::Impl::current;
    if(!result) throw std::logic_error("Embedded Tracy graphics adapter is unavailable");
    return *result;
}
}

namespace tracy
{
#include "TracyDxt1.inl"
bool EmbeddedInputAllowed() { return viewer::TracyGraphics().inputAllowed; }
void InitTexture()
{
    if(zigzagTex) return;
    const unsigned char* assets[] = {reinterpret_cast<const unsigned char*>(ZigZag32_data), reinterpret_cast<const unsigned char*>(ZigZag16_data),
        reinterpret_cast<const unsigned char*>(ZigZag08_data), reinterpret_cast<const unsigned char*>(ZigZag04_data),
        reinterpret_cast<const unsigned char*>(ZigZag02_data), reinterpret_cast<const unsigned char*>(ZigZag01_data)};
    const int sizes[] = {ZigZag32_size, ZigZag16_size, ZigZag08_size, ZigZag04_size, ZigZag02_size, ZigZag01_size};
    void* pixels[6]{}; int widths[6]{}, heights[6]{}, channels;
    try
    {
        for(int i=0; i<6; ++i)
        {
            pixels[i] = stbi_load_from_memory(assets[i], sizes[i], widths+i, heights+i, &channels, 4);
            if(!pixels[i]) throw std::runtime_error("Tracy zigzag image decode failed");
        }
        UpdateTextureRGBAMips(zigzagTex, pixels, widths, heights, 6);
    }
    catch(...) { for(auto* p : pixels) stbi_image_free(p); throw; }
    for(auto* p : pixels) stbi_image_free(p);
}
ImTextureID MakeTexture(bool) { viewer::TracyGraphics().CheckThread(); return 0; }
void FreeTexture(ImTextureID id, void(*run)(const std::function<void()>&, bool))
{
    run([id]{ viewer::TracyGraphics().Retire(id); }, false);
}
void UpdateTextureRGBA(ImTextureID& id, void* pixels, int width, int height)
{
    viewer::TracyGraphics().Upload(id, &pixels, &width, &height, 1);
}
void UpdateTextureRGBAMips(ImTextureID& id, void** pixels, int* width, int* height, size_t count)
{
    viewer::TracyGraphics().Upload(id, pixels, width, height, count);
}
void UpdateTexture(ImTextureID& id, const char* data, int width, int height)
{
    if(!data || width <= 0 || height <= 0 || width % 4 || height % 4 || uint64_t(width)*height*4 > UINT32_MAX)
        throw std::invalid_argument("Invalid Tracy DXT1 image");
    std::vector<uint32_t> rgba(size_t(width)*height);
    for(int y=0; y<height; y+=4) for(int x=0; x<width; x+=4)
    {
        uint64_t block; std::memcpy(&block, data, sizeof(block)); data += sizeof(block);
        DecodeDxt1Part(block, rgba.data()+size_t(y)*width+x, uint32_t(width));
    }
    UpdateTextureRGBA(id, rgba.data(), width, height);
}
}
