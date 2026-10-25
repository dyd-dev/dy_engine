#include "../../Platform/Profiler/CollectorInternal.h"
#include "VulkanDevice.h"
#include "../../Platform/Profiler/RhiProfiler.h"
#include "VulkanQuery.h"
#include "VulkanPresentQueue.h"
#include "../../RHI/WakeEvent.h"
#include <cstdio>

#include "dyf/RHI/Buffer.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/Pipeline.h"
#include "dyf/RHI/Binding.h"
#include "dyf/RHI/Shader.h"
#include "dyf/RHI/Texture.h"
#include "dyf/RHI/Readback.h"
#include "VulkanCommandList.h"
#include "VulkanContext.h"
#include "VulkanPipeline.h"
#include "VulkanResources.h"
#include "VulkanSwapchain.h"

#if !defined(__ANDROID__)
#include <GLFW/glfw3.h>
#else
#include <vulkan/vulkan_android.h>
#include <android/native_window.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__) && defined(DY_PROFILER_TRACING_ENABLED)
#include <time.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#endif

namespace dyf::Backends
{
	struct VulkanObjectDeleter
	{
		template<typename Object>
		void operator()(Object* object) const
		{
			delete object;
		}
	};

	namespace
	{
		constexpr const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";

		void LogVulkanFailure(const char* operation, VkResult result)
		{
			std::fprintf(stderr, "[Vulkan] %s failed (VkResult %d, deviceLost=%s)\n", operation, static_cast<int>(result), result == VK_ERROR_DEVICE_LOST ? "true" : "false");
		}

        VKAPI_ATTR VkBool32 VKAPI_CALL ValidationMessage(
            VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
            const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
        {
            const char* level = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "Error" : "Warning";
            std::fprintf(stderr, "[Vulkan.Validation][%s] %s\n", level,
                data && data->pMessage ? data->pMessage : "Unknown validation message");
            return VK_FALSE;
        }

		bool ValidationLayerAvailable()
		{
			uint32_t count = 0;
			if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) return false;
			std::vector<VkLayerProperties> layers(count);
			if (count > 0 && vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) return false;
			return std::any_of(layers.begin(), layers.end(), [](const VkLayerProperties& layer) {
				return std::strcmp(layer.layerName, kValidationLayerName) == 0;
			});
		}

		VkPresentModeKHR ToPresentMode(dyf::RHI::PresentMode mode)
		{
			switch (mode)
			{
			case dyf::RHI::PresentMode::Immediate: return VK_PRESENT_MODE_IMMEDIATE_KHR;
			case dyf::RHI::PresentMode::Mailbox: return VK_PRESENT_MODE_MAILBOX_KHR;
			case dyf::RHI::PresentMode::Fifo: return VK_PRESENT_MODE_FIFO_KHR;
			default: return VK_PRESENT_MODE_MAX_ENUM_KHR;
			}
		}
	}

	struct VulkanDevice::Impl
	{
		explicit Impl(VulkanDevice& owner) : m_owner(owner) {}
		~Impl();
        const VulkanContext& Context() const {return m_context;}
        RHI::UploadMemoryStatistics GetUploadMemoryStatistics() const
        {return m_uploadPool?m_uploadPool->GetStatistics():RHI::UploadMemoryStatistics{};}
        bool SetUploadMemoryPolicy(bool reusePages,bool suballocate)
        {
            if(!m_context.device||m_submissionFaulted)return false;
            if(!m_uploadPool)m_uploadPool=CreateVulkanUploadPool(m_context);
            m_uploadPool->SetPolicy({reusePages,suballocate});return true;
        }


		bool IsLost() const {return m_submissionFaulted || m_presentDeviceLost.load(std::memory_order_acquire) ||
            (m_commandPools && m_commandPools->IsDeviceLost());}
        bool Supports(RHI::Feature feature) const
        {
            if(!m_context.physicalInfo) return false;
            const auto& supported=m_context.physicalInfo->features;
            switch(feature)
            {
            case RHI::Feature::Compute:
            case RHI::Feature::IndirectDispatch:
                return (m_context.physicalInfo->graphicsQueue.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
            case RHI::Feature::Rasterization:
            case RHI::Feature::IndirectDraw:
            case RHI::Feature::SamplerLodBias:
            case RHI::Feature::FractionalDepthBias: return true;
            case RHI::Feature::Tessellation:
            case RHI::Feature::IndirectTessellation: return supported.tessellationShader == VK_TRUE;
            case RHI::Feature::IndirectFirstInstance: return supported.drawIndirectFirstInstance == VK_TRUE;
            case RHI::Feature::DescriptorIndexing: return supported.shaderSampledImageArrayDynamicIndexing == VK_TRUE;
            case RHI::Feature::Wireframe: return supported.fillModeNonSolid == VK_TRUE;
            case RHI::Feature::DepthBiasClamp: return supported.depthBiasClamp == VK_TRUE;
            case RHI::Feature::MeshShader: return m_context.meshShaderSupported;
            case RHI::Feature::TaskShader: return false;
            default: return false;
            }
        }
        uint64_t GetLastSubmission() const {return m_lastSubmission;}
        uint64_t GetCompletedSubmission() {CollectCompletedSubmissions(); return m_submissionFaulted?0:m_completedSubmission;}
        void DiscardCommandList(RHI::ICommandList* list)
        {
            const auto found=std::find_if(m_acquiredCommandLists.begin(),m_acquiredCommandLists.end(),
                [list](const auto& value) {return value.get()==list;});
            if(found==m_acquiredCommandLists.end())return;
            if(!IsLost() && (*found)->IsValid())(*found)->AllowPoolReuse();
            m_acquiredCommandLists.erase(found);
        }
        VkResult WaitForShutdown()
        {
            StopPresentWorker();
            if(m_context.device==VK_NULL_HANDLE)return VK_SUCCESS;
            const auto acquired=WaitForAcquisitions();
            if(acquired!=VK_SUCCESS && acquired!=VK_ERROR_DEVICE_LOST)return acquired;
            const auto idle=vkDeviceWaitIdle(m_context.device);
            if(idle!=VK_SUCCESS || !m_presentFencesEnabled)return idle;
            constexpr uint64_t timeout=1000000000ull;
            for(size_t i=0;i<m_presentFences.size();++i)if(m_presentFencePending[i])
            {
                const auto result=vkWaitForFences(m_context.device,1,&m_presentFences[i],VK_TRUE,timeout);
                if(result!=VK_SUCCESS)return result;
            }
            for(const auto& generation:m_retiredSwapchains)
                for(size_t i=0;i<generation.presentFences.size();++i)if(generation.presentFencePending[i])
                {
                    const auto result=vkWaitForFences(m_context.device,1,&generation.presentFences[i],VK_TRUE,timeout);
                    if(result!=VK_SUCCESS)return result;
                }
            return VK_SUCCESS;
        }
        bool WaitIdle()
        {
            if(m_context.device==VK_NULL_HANDLE) return false;
            const bool presented=PollPresentJobs(true);
            const auto acquired=WaitForAcquisitions();
            if(acquired!=VK_SUCCESS) return false;
            const VkResult result=vkDeviceWaitIdle(m_context.device);
            if(result==VK_ERROR_DEVICE_LOST) m_submissionFaulted=true;
            if(result!=VK_SUCCESS) return false;
            CollectCompletedSubmissions();
            return presented && !m_submissionFaulted;
        }
        bool ReadTimestamps(VulkanTimestampQuery& query,uint32_t first,uint32_t count,uint64_t* ticks)
        {
            CollectCompletedSubmissions();
            if(m_submissionFaulted || !std::all_of(query.initialized.begin()+first,query.initialized.begin()+first+count,
                [](uint8_t initialized) { return initialized!=0; })) return false;
            const VkResult result=vkGetQueryPoolResults(m_context.device,query.pool,first,count,
                size_t(count)*sizeof(uint64_t),ticks,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT);
            if(result==VK_ERROR_DEVICE_LOST) m_submissionFaulted=true;
            return result==VK_SUCCESS;
        }
        bool GetTimestampCalibration(RHI::TimestampCalibration& calibration)
        {
#if defined(_WIN32)
            if(m_submissionFaulted || !m_context.getCalibratedTimestamps || !m_context.cpuTimestampFrequency) return false;
            const VkCalibratedTimestampInfoEXT clocks[2] = {
                {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr, VK_TIME_DOMAIN_DEVICE_EXT},
                {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr, VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT}};
            uint64_t ticks[2]{}, deviation = 0;
            const VkResult result = m_context.getCalibratedTimestamps(m_context.device, 2, clocks, ticks, &deviation);
            if(result == VK_ERROR_DEVICE_LOST) m_submissionFaulted = true;
            if(result != VK_SUCCESS || !deviation) return false;
            calibration = {};
            calibration.cpuDomain = RHI::CpuTimestampDomain::QueryPerformanceCounter;
            calibration.cpuTicks = ticks[1];
            calibration.cpuFrequency = m_context.cpuTimestampFrequency;
            calibration.gpuTicks = ticks[0];
            calibration.gpuPeriodNanoseconds = m_owner.GetTimestampPeriodNanoseconds();
            calibration.gpuValidBits = m_owner.GetTimestampValidBits();
            calibration.maximumDeviationNanoseconds = deviation;
            return true;
#else
            (void)calibration;
            return false;
#endif
        }
        void ClearSwapchain()
        {
            PollPresentJobs(true);
            DestroyCurrentSwapchain();DestroyRetiredSwapchains();m_hasSwapchainDesc=false;
        }

		int Initialize(const void* windowHandle, const dyf::RHI::DeviceDesc& desc);
		bool CreateSwapchain(const dyf::RHI::SwapchainDesc& desc, VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE);
		bool BeginFrame();
		dyf::RHI::ICommandList* AcquireCommandList();
        RHI::ICommandList* AcquireCommandList(RHI::Detail::UploadPolicy);
		bool Submit(dyf::RHI::ICommandList** commandLists, uint32_t count);
		bool Present();
		dyf::RHI::TextureHandle GetBackBuffer() const { return m_backBuffer; }
		bool ReadTexture(dyf::RHI::TextureHandle texture, dyf::RHI::TextureReadback& result);

		dyf::RHI::BufferHandle CreateBuffer(const dyf::RHI::BufferDesc& desc);
		dyf::RHI::TextureHandle CreateTexture(const dyf::RHI::TextureDesc& desc);
		dyf::RHI::ShaderHandle CreateShader(const dyf::RHI::ShaderDesc& desc);
		RHI::PipelineHandle CreateComputePipeline(const RHI::ComputePipelineDesc& desc)
		{
			try
			{
				std::unique_ptr<VulkanPipeline, VulkanObjectDeleter> pipeline(new VulkanPipeline(m_context, desc));
				m_pipelines.push_back(pipeline.get());
				return pipeline.release();
			}
			catch (const std::exception&) { return nullptr; }
		}
        dyf::RHI::PipelineHandle CreateGraphicsPipeline(const dyf::RHI::GraphicsPipelineDesc& desc);
        dyf::RHI::PipelineHandle CreateMeshPipeline(const dyf::RHI::MeshPipelineDesc& desc)
        {
            try
            {
                std::unique_ptr<VulkanPipeline, VulkanObjectDeleter> pipeline(new VulkanPipeline(m_context, desc));
                m_pipelines.push_back(pipeline.get());
                return pipeline.release();
            }
            catch(const std::exception&) { return nullptr; }
        }
		dyf::RHI::ResourceSetHandle CreateResourceSet(const dyf::RHI::ResourceSetDesc& desc);
		void DestroyBuffer(dyf::RHI::BufferHandle buffer);
		void DestroyTexture(dyf::RHI::TextureHandle texture);
		void DestroyShader(dyf::RHI::ShaderHandle shader);
		void DestroyPipeline(dyf::RHI::PipelineHandle pipeline);
		void DestroyResourceSet(dyf::RHI::ResourceSetHandle resourceSet);
		bool UpdateBuffer(dyf::RHI::ICommandList& commandList, dyf::RHI::BufferHandle buffer, uint32_t offset, const void* data, uint32_t size);
		bool UpdateTexture(
			dyf::RHI::ICommandList& commandList,
			dyf::RHI::TextureHandle texture,
			uint32_t mipLevel,
			uint32_t arrayLayer,
			const void* data,
			uint32_t dataSize,
			uint32_t rowPitch,
			uint32_t slicePitch);

	private:
		VulkanDevice& m_owner;
		bool CreateInstance();
		VkResult CreateSurface();
		bool ReportSwapchainFailure(const VulkanSwapchain::InitializationStatus& status);
		bool PickPhysicalDevice();
		bool CreateLogicalDevice();
		bool CreateSwapchainSyncObjects();
		bool RecreateSwapchain();
		VkSwapchainKHR RetireSwapchainGeneration();
		void UpdateBackBuffers();
		void CollectCompletedSubmissions();
        VkResult AcquireSubmissionFence(VkFence& fence);
        void RecycleSubmissionFence(VkFence fence) noexcept;
		void DestroySwapchainSyncObjects();
		void DestroyCurrentSwapchain();
        void DestroyRetiredSwapchains(bool completedOnly=false);
		void DestroyDeviceResources();
        struct PresentJob
        {
            VkSwapchainKHR swapchain=VK_NULL_HANDLE;
            VkSemaphore waitSemaphore=VK_NULL_HANDLE;
            VkFence fence=VK_NULL_HANDLE;
            uint32_t imageIndex=0;
            bool acquireSuboptimal=false;
            VkResult result=VK_NOT_READY;
            double wallMilliseconds=0,cpuMilliseconds=-1;
            std::atomic<bool> ready{false};
        };
        void PresentWorkerLoop();
        void ConfigurePresentWorker();
        void StopPresentWorker();
        bool PollPresentJobs(bool wait=false);
        bool ApplyPresentResult(const PresentJob&);
        static void PublishPresentMetrics(const PresentJob&);
        void CallPresent(PresentJob&) noexcept;
        void TryPrefetchImage();
        VkResult AcquireImage(uint32_t slot,uint64_t timeout,uint32_t& image,bool prefetch=false);
        VkResult WaitForAcquisitions(uint64_t timeout=1000000000ull);
        static void WakePresentThread(std::atomic<bool>& sleeping,RHI::Detail::WakeEvent& wake)
        {
            if(sleeping.exchange(false,std::memory_order_acq_rel))
                wake.Signal();
        }

		VulkanContext m_context;
		std::shared_ptr<VulkanUploadPool> m_uploadPool;
        std::shared_ptr<VulkanCommandPoolCache> m_commandPools;
		VulkanSwapchain m_swapchain;
		void* m_windowHandle = nullptr;
        VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
        bool m_surfaceMaintenanceEnabled=false;
        bool m_presentFencesEnabled=false;
        bool m_swapchainPresented=false;
        bool m_asyncPresent=false,m_presentConfigured=false;
        std::unique_ptr<VulkanPresentQueue<PresentJob*>> m_presentQueue;
        std::deque<std::unique_ptr<PresentJob>> m_presentJobs;
        std::thread m_presentThread;
        std::atomic<bool> m_presentStop{false},m_presentSleeping{false},m_presentResultSleeping{false};
        std::atomic<bool> m_presentDeviceLost{false};
        RHI::Detail::WakeEvent m_presentWake,m_presentResultWake;
        bool m_prefetchedImage=false,m_prefetchedSuboptimal=false;
        uint32_t m_prefetchedImageIndex=0,m_prefetchedFrameSlot=0;
        uint64_t m_prefetchSuccess=0,m_prefetchNotReady=0,m_prefetchTimeout=0;
        uint64_t m_prefetchSlotBusy=0,m_prefetchAcquireFenceBusy=0;
        uint64_t m_prefetchCalls=0;
        double m_prefetchWallMilliseconds=0,m_prefetchMaxMilliseconds=0;

		std::vector<VkSemaphore> m_imageAvailableSemaphores;
        std::vector<VkFence> m_acquireFences;
        std::vector<uint8_t> m_acquireFencePending;
		std::vector<VkSemaphore> m_renderFinishedSemaphores;
        std::vector<VkFence> m_presentFences;
        std::vector<uint8_t> m_presentFencePending;
		std::vector<VkFence> m_frameSlots;
		std::vector<VkFence> m_imagesInFlight;

		uint64_t m_lastSubmission=0, m_completedSubmission=0;
        struct SubmissionRecord
		{
            uint64_t serial=0;
			VkFence fence = VK_NULL_HANDLE;
			std::vector<std::unique_ptr<VulkanCommandList, VulkanObjectDeleter>> commandLists;
			uint32_t frameSlot = std::numeric_limits<uint32_t>::max();
			uint32_t imageIndex = std::numeric_limits<uint32_t>::max();
		};
		std::vector<SubmissionRecord> m_submissions;
        std::vector<VkFence> m_availableFences;
		std::vector<std::unique_ptr<VulkanCommandList, VulkanObjectDeleter>> m_acquiredCommandLists;
		struct ReadbackResources
		{
			VkDevice device = VK_NULL_HANDLE;
			VkBuffer buffer = VK_NULL_HANDLE;
			VkDeviceMemory memory = VK_NULL_HANDLE;
			VkCommandPool pool = VK_NULL_HANDLE;
			VulkanTexture* source = nullptr;
			std::unique_ptr<VulkanTexture, VulkanObjectDeleter> retiredSource;
			~ReadbackResources()
			{
				if(pool) vkDestroyCommandPool(device, pool, nullptr);
				if(buffer) vkDestroyBuffer(device, buffer, nullptr);
				if(memory) vkFreeMemory(device, memory, nullptr);
			}
		};
		// An uncertain completion faults the device and prevents another readback.
		// Keep its resources until shutdown has established idle or device loss.
		std::unique_ptr<ReadbackResources> m_failedReadback;

		struct RetiredSwapchainGeneration
		{
			VulkanSwapchain swapchain;
			std::vector<VkSemaphore> imageAvailableSemaphores;
			std::vector<VkSemaphore> renderFinishedSemaphores;
            std::vector<VkFence> presentFences;
            std::vector<uint8_t> presentFencePending;
            bool presented=false;
			std::vector<std::unique_ptr<VulkanTexture, VulkanObjectDeleter>> backBuffers;
		};
		std::vector<RetiredSwapchainGeneration> m_retiredSwapchains;
		// 새 swapchain의 첫 present가 끝났다는 증거: 같은 이미지의 acquire를 기다린 제출.
		// https://docs.vulkan.org/samples/latest/samples/api/swapchain_recreation/README.html
		// 재획득 전에 다시 resize되면 다음 세대에서 증거를 얻을 때까지 여러 세대를 보유한다.
		uint32_t m_retiredReleaseImage = UINT32_MAX;
		bool m_retiredReleaseAcquired = false;
		uint64_t m_retiredReleaseSubmission = 0;

		std::vector<dyf::RHI::BufferHandle> m_buffers;
		std::vector<dyf::RHI::TextureHandle> m_textures;
		std::vector<dyf::RHI::ShaderHandle> m_shaders;
		std::vector<dyf::RHI::PipelineHandle> m_pipelines;
		std::vector<dyf::RHI::ResourceSetHandle> m_resourceSets;

		std::vector<std::unique_ptr<VulkanTexture, VulkanObjectDeleter>> m_backBuffers;
		dyf::RHI::TextureHandle m_backBuffer = nullptr;
		dyf::RHI::SwapchainDesc m_swapchainDesc = {};
		VkSwapchainKHR m_recreationOldSwapchain = VK_NULL_HANDLE;
		uint32_t m_currentFrameSlot = 0;
		uint32_t m_currentImageIndex = 0;
		bool m_hasSwapchainDesc = false;
		bool m_frameReady = false;
		bool m_imageAcquired = false;
		bool m_presentPending = false;
		bool m_swapchainNeedsRecreate = false;
		bool m_recreateAfterPresent = false;
		bool m_submissionFaulted = false;
		std::chrono::steady_clock::time_point m_frameUnavailableSince{};
		bool m_frameUnavailableWarned = false;
	};

	VulkanDevice::VulkanDevice()
		: m_impl(std::make_unique<Impl>(*this))
	{
	}

	VulkanDevice::~VulkanDevice()
	{
        StopLiveProfiling();
		const VkResult result = m_impl->WaitForShutdown();
		if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST)
		{
            LogVulkanFailure("Vulkan shutdown completion", result);
			AbandonResources();
			(void)m_impl.release();
			return;
		}
		ReleaseResources();
	}


RHI::UploadMemoryStatistics VulkanDevice::GetUploadMemoryStatisticsNative() const
{return m_impl->GetUploadMemoryStatistics();}
bool VulkanDevice::SetUploadMemoryPolicyNative(bool reusePages,bool suballocate)
{return m_impl->SetUploadMemoryPolicy(reusePages,suballocate);}
bool VulkanDevice::GetTimestampCalibrationNative(RHI::TimestampCalibration& calibration) const
{return m_impl->GetTimestampCalibration(calibration);}
uint64_t VulkanDevice::GetLastSubmissionNative() const {return m_impl->GetLastSubmission();}
uint64_t VulkanDevice::GetCompletedSubmissionNative() {return m_impl->GetCompletedSubmission();}
void VulkanDevice::DiscardCommandListNative(RHI::ICommandList* list) {m_impl->DiscardCommandList(list);}

bool VulkanDevice::SupportsNative(RHI::Feature feature) const
{
    if(feature==RHI::Feature::TimestampQuery)return GetTimestampValidBitsNative()!=0;
    if(feature==RHI::Feature::TimestampCalibration)return m_impl->Context().getCalibratedTimestamps&&m_impl->Context().cpuTimestampFrequency&&GetTimestampValidBitsNative();
    return m_impl->Supports(feature);
}
RHI::TimestampQueryHandle VulkanDevice::CreateTimestampQueryNative(const RHI::TimestampQueryDesc& desc)
{
    try {return new VulkanTimestampQuery(m_impl->Context().device,desc.count);}
    catch(const std::exception&) {return nullptr;}
}
void VulkanDevice::DestroyTimestampQueryNative(RHI::TimestampQueryHandle query)
{delete static_cast<VulkanTimestampQuery*>(query);}
bool VulkanDevice::ReadTimestampsNative(RHI::TimestampQueryHandle query,uint32_t first,uint32_t count,uint64_t* ticks)
{
    return m_impl->ReadTimestamps(*static_cast<VulkanTimestampQuery*>(query),first,count,ticks);
}
double VulkanDevice::GetTimestampPeriodNative() const
{
    const auto& info=m_impl->Context().physicalInfo;
    return info ? info->properties.limits.timestampPeriod : 0;
}
uint32_t VulkanDevice::GetTimestampValidBitsNative() const
{
    const auto& info=m_impl->Context().physicalInfo;
    return info ? info->graphicsQueue.timestampValidBits : 0;
}
bool VulkanDevice::IsLostNative() const {return m_impl->IsLost();}
bool VulkanDevice::WaitIdleNative() { return m_impl->WaitIdle(); }
void VulkanDevice::DestroySwapchainNative() { m_impl->ClearSwapchain(); }


    uint64_t VulkanDevice::GetLimitNative(RHI::Limit limit) const
    {
        const auto& info=m_impl->Context().physicalInfo;
        if(!info) return 0;
        const auto& limits=info->properties.limits;
        switch(limit)
        {
        case RHI::Limit::InlineConstantBytes: return limits.maxPushConstantsSize;
        case RHI::Limit::Texture2DDimension: return limits.maxImageDimension2D;
        case RHI::Limit::TextureArrayLayers: return limits.maxImageArrayLayers;
        case RHI::Limit::UniformBufferOffsetAlignment: return limits.minUniformBufferOffsetAlignment;
        case RHI::Limit::StorageBufferOffsetAlignment: return limits.minStorageBufferOffsetAlignment;
        case RHI::Limit::UniformBufferBytes: return limits.maxUniformBufferRange;
        case RHI::Limit::StorageBufferBytes: return limits.maxStorageBufferRange;
        case RHI::Limit::SamplerAnisotropy: {
            return info->features.samplerAnisotropy ? static_cast<uint64_t>(limits.maxSamplerAnisotropy) : 1;
        }
        case RHI::Limit::TessellationPatchControlPoints: return limits.maxTessellationPatchSize;
        }
        return 0;
    }

    bool VulkanDevice::SupportsSamplerNative(const RHI::SamplerDesc& desc) const
    {
        const auto& info=m_impl->Context().physicalInfo;
        return info && std::abs(desc.mipLodBias) <= info->properties.limits.maxSamplerLodBias;
    }

    bool VulkanDevice::SupportsPipelineLayoutNative(const RHI::PipelineLayoutDesc& desc) const
    {
        const auto& info=m_impl->Context().physicalInfo;
        if(!info) return false;
        const auto& limits=info->properties.limits;
        const auto& features=info->features;
        // 레이아웃 전체와 각 셰이더 단계의 실제 디스크립터 수를 네이티브 한도와 비교한다.
        uint64_t counts[7][5]={};
        for(uint32_t i=0;i<desc.bindingCount;++i)
        {
            const auto& binding=desc.bindings[i];
            uint32_t kind=0;
            switch(binding.type)
            {
            case RHI::ResourceBindingType::StaticSampler: kind=0;break;
            case RHI::ResourceBindingType::ConstantBuffer: kind=1;break;
            case RHI::ResourceBindingType::ReadOnlyStorageBuffer:
            case RHI::ResourceBindingType::ReadWriteStorageBuffer: kind=2;break;
            case RHI::ResourceBindingType::SampledTexture: kind=3;break;
            case RHI::ResourceBindingType::StorageTexture: kind=4;break;
            default:return false;
            }
            if(binding.type==RHI::ResourceBindingType::ReadWriteStorageBuffer ||
                binding.type==RHI::ResourceBindingType::StorageTexture)
            {
                if((binding.stages&(RHI::ShaderStageFlags::Vertex | RHI::ShaderStageFlags::Hull |
                    RHI::ShaderStageFlags::Domain))!=RHI::ShaderStageFlags::None &&
                    !features.vertexPipelineStoresAndAtomics)return false;
                if((binding.stages&RHI::ShaderStageFlags::Fragment)!=RHI::ShaderStageFlags::None &&
                    !features.fragmentStoresAndAtomics)return false;
            }
            counts[0][kind]+=binding.count;
            constexpr RHI::ShaderStageFlags stages[] = {
                RHI::ShaderStageFlags::Vertex, RHI::ShaderStageFlags::Hull,
                RHI::ShaderStageFlags::Domain, RHI::ShaderStageFlags::Fragment,
                RHI::ShaderStageFlags::Compute, RHI::ShaderStageFlags::Mesh};
            for(uint32_t stage=0;stage<6;++stage)
                if((binding.stages&stages[stage])!=RHI::ShaderStageFlags::None) counts[stage+1][kind]+=binding.count;
        }
        const uint64_t totalLimits[]={limits.maxDescriptorSetSamplers,limits.maxDescriptorSetUniformBuffers,
            limits.maxDescriptorSetStorageBuffers,limits.maxDescriptorSetSampledImages,limits.maxDescriptorSetStorageImages};
        const uint64_t stageLimits[]={limits.maxPerStageDescriptorSamplers,limits.maxPerStageDescriptorUniformBuffers,
            limits.maxPerStageDescriptorStorageBuffers,limits.maxPerStageDescriptorSampledImages,limits.maxPerStageDescriptorStorageImages};
        for(uint32_t kind=0;kind<5;++kind) if(counts[0][kind]>totalLimits[kind])return false;
        for(uint32_t stage=1;stage<7;++stage)
        {
            uint64_t resources=0;
            for(uint32_t kind=0;kind<5;++kind)
            {
                if(counts[stage][kind]>stageLimits[kind])return false;
                if(kind!=0)resources+=counts[stage][kind];
            }
            if(resources>limits.maxPerStageResources)return false;
        }
        return true;
    }

    bool VulkanDevice::SupportsLinearFiltering(RHI::Format format) const noexcept
    {
        const auto& context=m_impl->Context();
        const auto nativeFormat=ToVulkanFormat(format);
        if(!context.physicalDevice || nativeFormat==VK_FORMAT_UNDEFINED) return false;
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(context.physicalDevice,nativeFormat,&properties);
        constexpr VkFormatFeatureFlags required=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        return (properties.optimalTilingFeatures&required)==required;
    }

    bool VulkanDevice::SupportsDdsCompression(RHI::Format format) const noexcept
    {
        const auto& context=m_impl->Context();
        return RHI::IsBlockCompressed(format) && context.physicalInfo &&
            context.physicalInfo->features.textureCompressionBC && SupportsLinearFiltering(format);
    }

    bool VulkanDevice::SupportsGraphicsPipelineNative(const RHI::GraphicsPipelineDesc& desc) const
    {
        const auto& context=m_impl->Context();
        if(!context.physicalInfo) return false;
        const auto physical=context.physicalDevice;
        const auto& limits=context.physicalInfo->properties.limits;
        if(desc.vertexBufferCount>limits.maxVertexInputBindings || desc.vertexAttributeCount>limits.maxVertexInputAttributes ||
            desc.colorAttachmentCount>limits.maxColorAttachments)return false;
        uint64_t fragmentResources=desc.colorAttachmentCount;
        for(uint32_t i=0;i<desc.layout.bindingCount;++i)
        {
            const auto& binding=desc.layout.bindings[i];
            if(binding.type!=RHI::ResourceBindingType::StaticSampler &&
                (binding.stages&RHI::ShaderStageFlags::Fragment)!=RHI::ShaderStageFlags::None)
                fragmentResources+=binding.count;
        }
        if(fragmentResources>limits.maxPerStageResources) return false;
        for(uint32_t i=0;i<desc.vertexBufferCount;++i)
            if(desc.vertexBuffers[i].binding>=limits.maxVertexInputBindings ||
                desc.vertexBuffers[i].stride>limits.maxVertexInputBindingStride)return false;
        const auto hasFormat=[&](RHI::Format format,VkFormatFeatureFlags feature,bool buffer) {
            VkFormatProperties supported{};
            vkGetPhysicalDeviceFormatProperties(physical,ToVulkanFormat(format),&supported);
            return ((buffer?supported.bufferFeatures:supported.optimalTilingFeatures)&feature)==feature;
        };
        for(uint32_t i=0;i<desc.vertexAttributeCount;++i)
        {
            const auto& attribute=desc.vertexAttributes[i];
            if(attribute.location>=limits.maxVertexInputAttributes || attribute.offset>limits.maxVertexInputAttributeOffset ||
                !hasFormat(attribute.format,VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT,true))return false;
        }
        const auto& features=context.physicalInfo->features;
        const auto sameBlend=[](const RHI::ColorAttachmentDesc& a,const RHI::ColorAttachmentDesc& b) {
            return a.writeMask==b.writeMask && a.blend.enabled==b.blend.enabled &&
                (!a.blend.enabled || (a.blend.sourceColor==b.blend.sourceColor && a.blend.destinationColor==b.blend.destinationColor &&
                a.blend.colorOp==b.blend.colorOp && a.blend.sourceAlpha==b.blend.sourceAlpha &&
                a.blend.destinationAlpha==b.blend.destinationAlpha && a.blend.alphaOp==b.blend.alphaOp));
        };
        for(uint32_t i=0;i<desc.colorAttachmentCount;++i)
        {
            const auto& attachment=desc.colorAttachments[i];
            if(i && !features.independentBlend && !sameBlend(attachment,desc.colorAttachments[0]))return false;
            VkFormatFeatureFlags required=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
            if(attachment.blend.enabled)required|=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
            if(!hasFormat(attachment.format,required,false))return false;
        }
        return desc.depthStencil.format==RHI::Format::Unknown ||
            hasFormat(desc.depthStencil.format,VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT,false);
    }

    bool VulkanDevice::SupportsMeshPipelineNative(const RHI::MeshPipelineDesc& desc) const
    {
        const auto& context=m_impl->Context();
        if (!context.meshShaderSupported || !context.physicalInfo) return false;
        const auto physical = context.physicalDevice;
        const auto& limits = context.physicalInfo->properties.limits;
        if (desc.colorAttachmentCount > limits.maxColorAttachments) return false;

        const auto hasFormat = [&](RHI::Format format, VkFormatFeatureFlags feature) {
            VkFormatProperties supported{};
            vkGetPhysicalDeviceFormatProperties(physical, ToVulkanFormat(format), &supported);
            return (supported.optimalTilingFeatures & feature) == feature;
        };
        const auto& features=context.physicalInfo->features;
        const auto sameBlend = [](const RHI::ColorAttachmentDesc& a, const RHI::ColorAttachmentDesc& b) {
            return a.writeMask == b.writeMask && a.blend.enabled == b.blend.enabled &&
                a.blend.sourceColor == b.blend.sourceColor && a.blend.destinationColor == b.blend.destinationColor &&
                a.blend.colorOp == b.blend.colorOp && a.blend.sourceAlpha == b.blend.sourceAlpha &&
                a.blend.destinationAlpha == b.blend.destinationAlpha && a.blend.alphaOp == b.blend.alphaOp;
        };
        for (uint32_t i = 0; i < desc.colorAttachmentCount; ++i)
        {
            const auto& attachment = desc.colorAttachments[i];
            if (i && !features.independentBlend && !sameBlend(attachment, desc.colorAttachments[0])) return false;
            VkFormatFeatureFlags required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
            if (attachment.blend.enabled) required |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
            if (!hasFormat(attachment.format, required)) return false;
        }
        return desc.depthStencil.format == RHI::Format::Unknown ||
            hasFormat(desc.depthStencil.format, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
    }

    bool VulkanDevice::CreateSwapchainNative(const dyf::RHI::SwapchainDesc& desc)
	{
		return m_impl->CreateSwapchain(desc);
	}

	bool VulkanDevice::BeginFrameNative()
	{
		return m_impl->BeginFrame();
	}

	dyf::RHI::ICommandList* VulkanDevice::AcquireCommandListNative()
	{
		return m_impl->AcquireCommandList();
	}
    RHI::ICommandList* VulkanDevice::AcquireCommandListNative(bool reusePages,bool suballocate)
    {return m_impl->AcquireCommandList({reusePages,suballocate});}

	bool VulkanDevice::SubmitNative(dyf::RHI::ICommandList** commandLists, uint32_t count)
	{
		return m_impl->Submit(commandLists, count);
	}

	bool VulkanDevice::PresentNative()
	{
		return m_impl->Present();
	}

	dyf::RHI::TextureHandle VulkanDevice::GetBackBufferNative()
	{
		return m_impl->GetBackBuffer();
	}

	dyf::RHI::BufferHandle VulkanDevice::CreateBufferNative(const dyf::RHI::BufferDesc& desc)
	{
		return m_impl->CreateBuffer(desc);
	}

	dyf::RHI::TextureHandle VulkanDevice::CreateTextureNative(const dyf::RHI::TextureDesc& desc)
	{
		return m_impl->CreateTexture(desc);
	}

	dyf::RHI::ShaderHandle VulkanDevice::CreateShaderNative(const dyf::RHI::ShaderDesc& desc)
	{
		return m_impl->CreateShader(desc);
	}

RHI::PipelineHandle VulkanDevice::CreateComputePipelineNative(const RHI::ComputePipelineDesc& desc) {return m_impl->CreateComputePipeline(desc);}
	dyf::RHI::PipelineHandle VulkanDevice::CreateGraphicsPipelineNative(const dyf::RHI::GraphicsPipelineDesc& desc)
	{
		return m_impl->CreateGraphicsPipeline(desc);
	}

	dyf::RHI::PipelineHandle VulkanDevice::CreateMeshPipelineNative(const dyf::RHI::MeshPipelineDesc& desc)
	{
		return m_impl->CreateMeshPipeline(desc);
	}

	dyf::RHI::ResourceSetHandle VulkanDevice::CreateResourceSetNative(const dyf::RHI::ResourceSetDesc& desc)
	{
		return m_impl->CreateResourceSet(desc);
	}

	void VulkanDevice::DestroyBufferNative(dyf::RHI::BufferHandle buffer)
	{
		m_impl->DestroyBuffer(buffer);
	}

	void VulkanDevice::DestroyTextureNative(dyf::RHI::TextureHandle texture)
	{
		m_impl->DestroyTexture(texture);
	}

	void VulkanDevice::DestroyShaderNative(dyf::RHI::ShaderHandle shader)
	{
		m_impl->DestroyShader(shader);
	}

	void VulkanDevice::DestroyPipelineNative(dyf::RHI::PipelineHandle pipeline)
	{
		m_impl->DestroyPipeline(pipeline);
	}

	void VulkanDevice::DestroyResourceSetNative(dyf::RHI::ResourceSetHandle resourceSet)
	{
		m_impl->DestroyResourceSet(resourceSet);
	}

	bool VulkanDevice::UpdateBufferNative(dyf::RHI::ICommandList& commandList, dyf::RHI::BufferHandle buffer, uint32_t offset, const void* data, uint32_t size)
	{
		return m_impl->UpdateBuffer(commandList, buffer, offset, data, size);
	}

	bool VulkanDevice::UpdateTextureNative(
		dyf::RHI::ICommandList& commandList,
		dyf::RHI::TextureHandle texture,
		uint32_t mipLevel,
		uint32_t arrayLayer,
		const void* data,
		uint32_t dataSize,
		uint32_t rowPitch,
		uint32_t slicePitch)
	{
		return m_impl->UpdateTexture(commandList, texture, mipLevel, arrayLayer, data, dataSize, rowPitch, slicePitch);
	}

	int VulkanDevice::Initialize(const void* windowHandle, const dyf::RHI::DeviceDesc& desc)
	{
		return m_impl->Initialize(windowHandle, desc);
	}

	VulkanDevice::Impl::~Impl()
	{
        StopPresentWorker();
		DestroyDeviceResources();
	}

	int VulkanDevice::Impl::Initialize(const void* windowHandle, const dyf::RHI::DeviceDesc& desc)
	{
		m_windowHandle = const_cast<void*>(windowHandle);
		if (desc.maxFramesInFlight == 0) return -1;

		if (!CreateInstance() || !PickPhysicalDevice() || !CreateLogicalDevice()) return -1;
		m_frameSlots.assign(m_owner.GetDesc().maxFramesInFlight, VK_NULL_HANDLE);
		return 0;
	}

	bool VulkanDevice::Impl::ReportSwapchainFailure(const VulkanSwapchain::InitializationStatus& status)
	{
		if (status.retryLater) return false;
		if (status.operation != nullptr)
		{
			m_submissionFaulted = true;
			LogVulkanFailure(status.operation, status.result);
		}
		else if (m_hasSwapchainDesc)
		{
			m_submissionFaulted = true;
			std::fprintf(stderr, "[Vulkan] Vulkan swapchain recreation cannot satisfy the requested configuration.\n");
		}
		return false;
	}

	bool VulkanDevice::Impl::CreateSwapchain(const dyf::RHI::SwapchainDesc& desc, VkSwapchainKHR oldSwapchain)
	{
        if(!PollPresentJobs(true))return false;
        if(m_windowHandle && m_windowHandle!=desc.window) return false;
        m_windowHandle=const_cast<void*>(desc.window);
        if(!m_windowHandle) return false;
        VulkanSwapchain::InitializationStatus status;
        if(m_context.surface==VK_NULL_HANDLE && !status.Check(CreateSurface(), "Vulkan surface creation"))
            return ReportSwapchainFailure(status);
        VkBool32 presentation=VK_FALSE;
        if(!status.Check(vkGetPhysicalDeviceSurfaceSupportKHR(m_context.physicalDevice,m_context.queueIndices.graphicsFamily,m_context.surface,&presentation), "vkGetPhysicalDeviceSurfaceSupportKHR") || !presentation)
            return ReportSwapchainFailure(status);
		if (m_context.device == VK_NULL_HANDLE || m_windowHandle == nullptr || m_submissionFaulted || desc.minimumImageCount == 0) return false;
		CollectCompletedSubmissions();
		if (m_submissionFaulted || !m_submissions.empty() || m_frameReady || m_imageAcquired || m_presentPending ||
			m_swapchain.GetHandle() != VK_NULL_HANDLE)
		{
			return false;
		}

		const VkFormat requestedFormat = ToVulkanFormat(desc.format);
		if (requestedFormat == VK_FORMAT_UNDEFINED) return false;
        VkColorSpaceKHR requestedColorSpace=VK_COLOR_SPACE_MAX_ENUM_KHR;
        switch(desc.colorSpace)
        {
        case RHI::ColorSpace::Srgb: requestedColorSpace=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;break;
        case RHI::ColorSpace::LinearSrgb: requestedColorSpace=VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;break;
        default:return false;
        }
        VkCompositeAlphaFlagBitsKHR requestedCompositeAlpha=VK_COMPOSITE_ALPHA_FLAG_BITS_MAX_ENUM_KHR;
        switch(desc.compositeAlpha)
        {
        case RHI::CompositeAlpha::Opaque: requestedCompositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;break;
        case RHI::CompositeAlpha::Premultiplied: requestedCompositeAlpha=VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;break;
        case RHI::CompositeAlpha::Postmultiplied: requestedCompositeAlpha=VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;break;
        case RHI::CompositeAlpha::Inherit: requestedCompositeAlpha=VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;break;
        default:return false;
        }
		const VkPresentModeKHR requestedPresentMode = ToPresentMode(desc.presentMode);
		if (requestedPresentMode == VK_PRESENT_MODE_MAX_ENUM_KHR) return false;

        ConfigurePresentWorker();
        const uint32_t preferredImages=m_asyncPresent
            ? static_cast<uint32_t>(std::min<uint64_t>(UINT32_MAX,std::max<uint64_t>(3,uint64_t(desc.minimumImageCount)+1)))
            : desc.minimumImageCount;
		bool oldSwapchainRetired = false;
		try
		{
			if (!m_swapchain.Initialize(
				m_context,
				m_windowHandle,
				requestedFormat,
                requestedColorSpace,
                requestedCompositeAlpha,
				requestedPresentMode,
                desc.minimumImageCount,
                preferredImages,
				desc.allowReadback,
				m_hasSwapchainDesc ? 0 : desc.initialWidth,
				m_hasSwapchainDesc ? 0 : desc.initialHeight,
				oldSwapchain,
				oldSwapchainRetired,
				status))
			{
				if (oldSwapchainRetired) m_recreationOldSwapchain = VK_NULL_HANDLE;
				return ReportSwapchainFailure(status);
			}
			if (oldSwapchainRetired) m_recreationOldSwapchain = VK_NULL_HANDLE;

			UpdateBackBuffers();
			if (!CreateSwapchainSyncObjects())
			{
				DestroyCurrentSwapchain();
				return false;
			}
		}
		catch (const std::exception& exception)
		{
			m_submissionFaulted = true;
			if (oldSwapchainRetired) m_recreationOldSwapchain = VK_NULL_HANDLE;
			DestroyCurrentSwapchain();
			std::fprintf(stderr, "[Vulkan] Vulkan swapchain creation failed: %s\n", exception.what());
			return false;
		}

		m_swapchainDesc = desc;
		m_hasSwapchainDesc = true;
		m_currentFrameSlot = 0;
		m_currentImageIndex = 0;
		m_frameReady = false;
		m_imageAcquired = false;
		m_presentPending = false;
		m_swapchainNeedsRecreate = false;
		m_recreateAfterPresent = false;
		m_retiredReleaseImage = UINT32_MAX;
		m_retiredReleaseAcquired = false;
		m_retiredReleaseSubmission = 0;
		m_swapchainPresented = false;
        m_prefetchedImage=false;
        std::fprintf(stderr,"[Vulkan] Swapchain images: requiredMinimum=%u preferred=%u actual=%zu.\n",
            desc.minimumImageCount,preferredImages,m_swapchain.GetImageCount());
		if (m_swapchain.GetImageCount() != preferredImages)
		{
			char message[192];
			std::snprintf(message, sizeof(message),
                "Vulkan swapchain requires at least %u images, prefers %u; the surface/driver allocated %zu.",
                desc.minimumImageCount, preferredImages, m_swapchain.GetImageCount());
			m_owner.ReportDiagnostic(DiagnosticSeverity::Info, message);
		}
		return true;
	}

    void VulkanDevice::Impl::ConfigurePresentWorker()
    {
        if(m_presentConfigured)return;
        m_presentConfigured=true;
#if defined(__linux__) && !defined(__ANDROID__)
        // Device creation has no window. Defer GLFW access until the caller
        // supplies a live GLFW window when creating its first swapchain.
        m_asyncPresent=glfwGetPlatform()==GLFW_PLATFORM_WAYLAND && m_owner.GetDesc().maxFramesInFlight>1 &&
            m_context.presentQueue!=m_context.graphicsQueue && m_owner.GetDesc().maxFramesInFlight<UINT32_MAX;
#endif
        if(m_asyncPresent)
        {
            try
            {
                m_presentQueue=std::make_unique<VulkanPresentQueue<PresentJob*>>(m_owner.GetDesc().maxFramesInFlight+1);
                m_presentThread=std::thread([this] {PresentWorkerLoop();});
            }
            catch(...) {m_asyncPresent=false;m_presentQueue.reset();}
        }
        std::fprintf(stderr,"[Vulkan] Present: %s; graphicsQueue=%p presentQueue=%p framesInFlight=%u.\n",
            m_asyncPresent?"asynchronous worker":"synchronous",static_cast<void*>(m_context.graphicsQueue),
            static_cast<void*>(m_context.presentQueue),m_owner.GetDesc().maxFramesInFlight);
    }

    VkResult VulkanDevice::Impl::AcquireImage(uint32_t slot,uint64_t timeout,uint32_t& image,bool prefetch)
    {
        VkFence fence=VK_NULL_HANDLE;
        if(m_asyncPresent)
        {
            fence=m_acquireFences[slot];
            if(m_acquireFencePending[slot])
            {
                const auto status=vkGetFenceStatus(m_context.device,fence);
                if(prefetch && status==VK_NOT_READY)++m_prefetchAcquireFenceBusy;
                if(status!=VK_SUCCESS)return status;
                m_acquireFencePending[slot]=0;
            }
            const auto reset=vkResetFences(m_context.device,1,&fence);
            if(reset!=VK_SUCCESS)return reset;
        }
        const auto result=vkAcquireNextImageKHR(m_context.device,m_swapchain.GetHandle(),timeout,
            m_imageAvailableSemaphores[slot],fence,&image);
        if(fence!=VK_NULL_HANDLE && (result==VK_SUCCESS || result==VK_SUBOPTIMAL_KHR))
            m_acquireFencePending[slot]=1;
        return result;
    }

    VkResult VulkanDevice::Impl::WaitForAcquisitions(uint64_t timeout)
    {
        for(size_t slot=0;slot<m_acquireFences.size();++slot)
            if(m_acquireFencePending[slot])
            {
                const auto result=vkWaitForFences(m_context.device,1,&m_acquireFences[slot],VK_TRUE,timeout);
                if(result!=VK_SUCCESS)
                {if(result==VK_ERROR_DEVICE_LOST)m_submissionFaulted=true;return result;}
                m_acquireFencePending[slot]=0;
            }
        return VK_SUCCESS;
    }

    void VulkanDevice::Impl::TryPrefetchImage()
    {
        if(!m_asyncPresent || m_prefetchedImage || m_swapchainNeedsRecreate)return;
        if(m_recreateAfterPresent)
        {
            // SUBOPTIMAL can remain an unchanged advisory on Wayland. Keep
            // pre-acquisition available until the surface actually needs a
            // new swapchain, just as ApplyPresentResult does after Present.
            bool changed=false;
            VulkanSwapchain::InitializationStatus status;
            if(!m_swapchain.IsSurfaceConfigurationChanged(m_context,m_windowHandle,changed,status) || changed)return;
        }
        CollectCompletedSubmissions();
        const uint32_t slot=(m_currentFrameSlot+1)%m_owner.GetDesc().maxFramesInFlight;
        if(m_submissionFaulted || slot==m_currentFrameSlot)return;
        if(m_frameSlots[slot]!=VK_NULL_HANDLE){++m_prefetchSlotBusy;return;}
        uint32_t image=0;
        // Allow a short buffer-release wait while current graphics work is running.
        const auto started=std::chrono::steady_clock::now();
        const auto result=AcquireImage(slot,1000000ull,image,true);
        const double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
        ++m_prefetchCalls;m_prefetchWallMilliseconds+=elapsed;
        m_prefetchMaxMilliseconds=std::max(m_prefetchMaxMilliseconds,elapsed);
        if(result==VK_SUCCESS || result==VK_SUBOPTIMAL_KHR)
        {
            ++m_prefetchSuccess;
            m_prefetchedImage=true;m_prefetchedImageIndex=image;m_prefetchedFrameSlot=slot;
            m_prefetchedSuboptimal=result==VK_SUBOPTIMAL_KHR;
        }
        else if(result==VK_NOT_READY)++m_prefetchNotReady;
        else if(result==VK_TIMEOUT)++m_prefetchTimeout;
        else if(result==VK_ERROR_OUT_OF_DATE_KHR)m_swapchainNeedsRecreate=true;
        else if(result!=VK_NOT_READY && result!=VK_TIMEOUT)
        {m_submissionFaulted=true;LogVulkanFailure("vkAcquireNextImageKHR(prefetch)",result);}
    }

    void VulkanDevice::Impl::CallPresent(PresentJob& job) noexcept
    {
        VkPresentInfoKHR info{};info.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        info.waitSemaphoreCount=1;info.pWaitSemaphores=&job.waitSemaphore;
        info.swapchainCount=1;info.pSwapchains=&job.swapchain;info.pImageIndices=&job.imageIndex;
        VkSwapchainPresentFenceInfoEXT fence{};
        if(job.fence!=VK_NULL_HANDLE)
        {
            fence.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT;
            fence.swapchainCount=1;fence.pFences=&job.fence;info.pNext=&fence;
        }
#ifdef DY_PROFILER_TRACING_ENABLED
        Platform::CpuTimer wall;
#if defined(__linux__)
        timespec begin{},end{};
        const bool hasBegin=clock_gettime(CLOCK_THREAD_CPUTIME_ID,&begin)==0;
#endif
#endif
        job.result=vkQueuePresentKHR(m_context.presentQueue,&info);
#ifdef DY_PROFILER_TRACING_ENABLED
#if defined(__linux__)
        const bool hasEnd=clock_gettime(CLOCK_THREAD_CPUTIME_ID,&end)==0;
#endif
        job.wallMilliseconds=wall.ElapsedMilliseconds();
#if defined(__linux__)
        if(hasBegin && hasEnd)job.cpuMilliseconds=1000.0*double(end.tv_sec-begin.tv_sec)+double(end.tv_nsec-begin.tv_nsec)/1e6;
#endif
#endif
    }

    void VulkanDevice::Impl::PresentWorkerLoop()
    {
        for(;;)
        {
            PresentJob* job=nullptr;
            if(m_presentQueue->TryPop(job))
            {
                CallPresent(*job);
                if(job->result==VK_ERROR_DEVICE_LOST)m_presentDeviceLost.store(true,std::memory_order_release);
                // This publication is the worker's final access to the job.
                job->ready.store(true,std::memory_order_release);
                WakePresentThread(m_presentResultSleeping,m_presentResultWake);
                continue;
            }
            m_presentSleeping.exchange(true,std::memory_order_acq_rel);
            // Arm before rechecking the queue. A signal racing with Wait remains
            // counted by the native wake object, so no publication can be lost.
            if(!m_presentStop.load(std::memory_order_acquire) && m_presentQueue->Empty())
                m_presentWake.Wait();
            m_presentSleeping.store(false,std::memory_order_release);
            if(m_presentStop.load(std::memory_order_acquire) && m_presentQueue->Empty())return;
        }
    }

    bool VulkanDevice::Impl::PollPresentJobs(bool wait)
    {
        bool succeeded=true;
        while(!m_presentJobs.empty())
        {
            auto& job=*m_presentJobs.front();
            while(!job.ready.load(std::memory_order_acquire))
            {
                if(!wait)return succeeded;
                m_presentResultSleeping.exchange(true,std::memory_order_acq_rel);
                // Recheck after arming, and tolerate a retained notification for
                // an earlier result before consuming this job's release publication.
                if(!job.ready.load(std::memory_order_acquire)) m_presentResultWake.Wait();
                m_presentResultSleeping.store(false,std::memory_order_release);
            }
            auto completed=std::move(m_presentJobs.front());
            m_presentJobs.pop_front();
            if(!ApplyPresentResult(*completed))succeeded=false;
            PublishPresentMetrics(*completed);
        }
        return succeeded;
    }

    void VulkanDevice::Impl::StopPresentWorker()
    {
        if(!m_presentThread.joinable())return;
        PollPresentJobs(true);
        m_presentStop.store(true,std::memory_order_release);
        WakePresentThread(m_presentSleeping,m_presentWake);
        m_presentThread.join();
        std::fprintf(stderr,"[Vulkan] Prefetch totals: success=%llu notReady=%llu timeout=%llu slotBusy=%llu acquireFenceBusy=%llu.\n",
            static_cast<unsigned long long>(m_prefetchSuccess),static_cast<unsigned long long>(m_prefetchNotReady),
            static_cast<unsigned long long>(m_prefetchTimeout),static_cast<unsigned long long>(m_prefetchSlotBusy),
            static_cast<unsigned long long>(m_prefetchAcquireFenceBusy));
        std::fprintf(stderr,"[Vulkan] Prefetch host time: calls=%llu mean=%.6f ms max=%.6f ms.\n",
            static_cast<unsigned long long>(m_prefetchCalls),m_prefetchCalls?m_prefetchWallMilliseconds/m_prefetchCalls:0,
            m_prefetchMaxMilliseconds);
    }

	bool VulkanDevice::Impl::BeginFrame()
	{
		if(!PollPresentJobs())return false;
		if (m_context.device == VK_NULL_HANDLE || m_submissionFaulted) return false;
		const auto unavailable = [&](const char* reason) {
			if (m_submissionFaulted) return false;
			const auto now = std::chrono::steady_clock::now();
			if (m_frameUnavailableSince == std::chrono::steady_clock::time_point{})
				m_frameUnavailableSince = now;
			if (!m_frameUnavailableWarned && now - m_frameUnavailableSince >= std::chrono::seconds(2))
			{
				m_frameUnavailableWarned = true;
				std::fprintf(stderr, "[Vulkan] No frame has started for at least 2 seconds: reason=%s lastSubmission=%llu completedSubmission=%llu pendingSubmissions=%zu retiredSwapchains=%zu frameSlot=%u/%zu image=%u/%zu acquired=%s\n",
					reason, static_cast<unsigned long long>(m_lastSubmission),
					static_cast<unsigned long long>(m_completedSubmission), m_submissions.size(), m_retiredSwapchains.size(),
					m_currentFrameSlot, m_frameSlots.size(), m_currentImageIndex, m_imagesInFlight.size(),
					m_imageAcquired ? "true" : "false");
			}
			return false;
		};
		const auto available = [&] {
			m_frameUnavailableSince = {};
			m_frameUnavailableWarned = false;
			return true;
		};
		CollectCompletedSubmissions();
		if (m_submissionFaulted) return false;
        if(!m_imageAcquired && m_prefetchedImage && !m_swapchainNeedsRecreate)
        {
            if(m_prefetchedFrameSlot!=m_currentFrameSlot) {m_submissionFaulted=true;return false;}
            m_currentImageIndex=m_prefetchedImageIndex;
            m_imageAcquired=true;m_recreateAfterPresent=m_prefetchedSuboptimal;m_prefetchedImage=false;
            if(!m_retiredSwapchains.empty() && m_currentImageIndex==m_retiredReleaseImage)
                m_retiredReleaseAcquired=true;
        }
		if (m_frameReady)
		{
			const bool ready = m_imageAcquired && m_swapchain.GetHandle() != VK_NULL_HANDLE &&
				m_currentFrameSlot < m_frameSlots.size() &&
				m_frameSlots[m_currentFrameSlot] == VK_NULL_HANDLE &&
				m_currentImageIndex < m_imagesInFlight.size() &&
				m_imagesInFlight[m_currentImageIndex] == VK_NULL_HANDLE &&
				m_currentImageIndex < m_backBuffers.size() &&
				m_backBuffer == m_backBuffers[m_currentImageIndex].get();
			if (ready) return available();
			if (m_currentFrameSlot < m_frameSlots.size() && m_frameSlots[m_currentFrameSlot] != VK_NULL_HANDLE)
				return unavailable("frame-slot-fence");
			if (m_currentImageIndex < m_imagesInFlight.size() && m_imagesInFlight[m_currentImageIndex] != VK_NULL_HANDLE)
				return unavailable("image-fence");
			return unavailable("frame-state");
		}

		// Wayland may keep presenting successfully while scaling buffers from the
		// previous window size. Check the required extent before acquiring another
		// image instead of relying exclusively on WSI advisory/error return codes.
		if (m_swapchain.GetHandle() != VK_NULL_HANDLE && (!m_imageAcquired || m_asyncPresent))
		{
			bool changed = false;
			VulkanSwapchain::InitializationStatus status;
			if (!m_swapchain.IsSurfaceConfigurationChanged(m_context, m_windowHandle, changed, status))
			{
				if (!status.retryLater) return ReportSwapchainFailure(status);
				return unavailable("surface-unavailable");
			}
			if (changed && (m_presentFencesEnabled || m_retiredSwapchains.empty())) m_swapchainNeedsRecreate = true;
		}

		if (m_swapchainNeedsRecreate)
		{
            if(m_asyncPresent && !m_frameReady && !m_presentPending && (m_prefetchedImage || m_imageAcquired))
            {
                const auto acquired=WaitForAcquisitions(0);
                if(acquired==VK_TIMEOUT)return unavailable("prefetch-acquire-fence");
                if(acquired!=VK_SUCCESS) {m_submissionFaulted=true;return false;}
                // These images were never submitted. Retire their semaphores with
                // the old generation instead of reusing a still-signaled semaphore.
                m_prefetchedImage=false;m_imageAcquired=false;
            }
			if (m_imageAcquired || !RecreateSwapchain())
				return unavailable("swapchain-recreate");
		}
		if (m_swapchain.GetHandle() == VK_NULL_HANDLE || m_currentFrameSlot >= m_frameSlots.size() ||
			m_frameSlots[m_currentFrameSlot] != VK_NULL_HANDLE)
		{
			return unavailable(m_currentFrameSlot < m_frameSlots.size() &&
				m_frameSlots[m_currentFrameSlot] != VK_NULL_HANDLE ? "frame-slot-fence" : "swapchain-unavailable");
		}

		if (!m_imageAcquired)
		{
            // Acquire and Present externally synchronize the same swapchain.
            // A prefetched image lets CPU recording run during the previous host Present.
            if(!m_presentJobs.empty())return unavailable("presentation-host-call");
			// Allow WSI to wait for the compositor's buffer release without an
			// unbounded wait that would prevent the window loop handling input.
			constexpr uint64_t acquireTimeoutNanoseconds = 16000000ull;
			const VkResult result=AcquireImage(m_currentFrameSlot,acquireTimeoutNanoseconds,m_currentImageIndex);
			if (result == VK_ERROR_OUT_OF_DATE_KHR)
			{
				m_swapchainNeedsRecreate = true;
				return unavailable("swapchain-recreate");
			}
			if (result == VK_NOT_READY) return unavailable("acquire-VK_NOT_READY");
			if (result == VK_TIMEOUT) return unavailable("acquire-VK_TIMEOUT");
			if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
			{
				m_submissionFaulted = true;
				LogVulkanFailure("vkAcquireNextImageKHR", result);
				return false;
			}
			m_imageAcquired = true;
			m_recreateAfterPresent = result == VK_SUBOPTIMAL_KHR;
			if (!m_retiredSwapchains.empty() && m_currentImageIndex == m_retiredReleaseImage)
				m_retiredReleaseAcquired = true;
		}

		if (m_currentImageIndex >= m_imagesInFlight.size() || m_currentImageIndex >= m_backBuffers.size())
		{
			m_submissionFaulted = true;
			return false;
		}
		if (m_imagesInFlight[m_currentImageIndex] != VK_NULL_HANDLE) return unavailable("image-fence");
        if(m_presentFencesEnabled)
        {
            const auto status=vkGetFenceStatus(m_context.device,m_presentFences[m_currentImageIndex]);
            if(status==VK_NOT_READY)return unavailable("presentation-fence");
            if(status!=VK_SUCCESS)
            {
                m_submissionFaulted=true;
                LogVulkanFailure("vkGetFenceStatus(presentation)",status);
                return false;
            }
        }

		m_backBuffer = m_backBuffers[m_currentImageIndex].get();
		m_frameReady = true;
        // RHI의 backbuffer 시작 상태 Present를 최초 Vulkan 이미지의 Undefined 레이아웃에 대응시킨다.
        if(m_backBuffers[m_currentImageIndex]->GetBarrierOldLayout(RHI::ResourceState::Present)==VK_IMAGE_LAYOUT_UNDEFINED)
        {
            auto* command=static_cast<VulkanCommandList*>(AcquireCommandList());
            if(!command) { m_frameReady=false;return unavailable("frame-initialization"); }
            RHI::ResourceBarrierDesc barrier{};
            barrier.texture=m_backBuffer;
            barrier.before=RHI::ResourceState::Present;
            barrier.after=RHI::ResourceState::Present;
            command->ResourceBarrierNative(&barrier,1);
            RHI::ICommandList* submitted=command;
            if(!command->CloseNative() || !Submit(&submitted,1))
            { m_submissionFaulted=true;return false; }
            m_backBuffers[m_currentImageIndex]->MarkLayoutInitialized();
        }
		return available();
	}

	dyf::RHI::ICommandList* VulkanDevice::Impl::AcquireCommandList()
    {return AcquireCommandList(m_uploadPool?m_uploadPool->GetPolicy():RHI::Detail::UploadPolicy{});}

	dyf::RHI::ICommandList* VulkanDevice::Impl::AcquireCommandList(RHI::Detail::UploadPolicy policy)
	{
        // BeginFrame and the RHI preparation batch already collected completions;
        // avoid polling the same pending fence again for each native list.
		if (m_context.device == VK_NULL_HANDLE || m_submissionFaulted) return nullptr;
		try
		{
			if (!m_uploadPool) m_uploadPool = CreateVulkanUploadPool(m_context);
            if(!m_commandPools)m_commandPools=std::make_shared<VulkanCommandPoolCache>(m_context);
			std::unique_ptr<VulkanCommandList, VulkanObjectDeleter> commandList(
				new VulkanCommandList(m_context, m_uploadPool, m_commandPools,policy));
			VulkanCommandList* result = commandList.get();
			m_acquiredCommandLists.push_back(std::move(commandList));
			return result;
		}
		catch (const std::exception& exception)
		{
			if(m_commandPools && m_commandPools->IsDeviceLost())m_submissionFaulted=true;
			std::fprintf(stderr, "[Vulkan] Vulkan command-list acquisition failed: %s\n", exception.what());
			return nullptr;
		}
	}

	bool VulkanDevice::Impl::Submit(dyf::RHI::ICommandList** commandLists, uint32_t count)
	{
		if (commandLists == nullptr || count == 0) return false;
		CollectCompletedSubmissions();

		std::vector<VulkanCommandList*> selected;
		selected.reserve(count);
		for (uint32_t i = 0; i < count; ++i)
		{
			if (commandLists[i] == nullptr ||
				std::find(commandLists, commandLists + i, commandLists[i]) != commandLists + i)
			{
				return false;
			}
			const auto acquired = std::find_if(
				m_acquiredCommandLists.begin(),
				m_acquiredCommandLists.end(),
				[requested = commandLists[i]](
					const std::unique_ptr<VulkanCommandList, VulkanObjectDeleter>& candidate) {
					return candidate.get() == requested;
				});
			if (acquired == m_acquiredCommandLists.end() || !(*acquired)->IsClosed()) return false;
			selected.push_back(acquired->get());
		}

		std::vector<std::unique_ptr<VulkanCommandList, VulkanObjectDeleter>> consumed;
		consumed.reserve(count);
		for (VulkanCommandList* commandList : selected)
		{
			const auto acquired = std::find_if(
				m_acquiredCommandLists.begin(),
				m_acquiredCommandLists.end(),
				[commandList](
					const std::unique_ptr<VulkanCommandList, VulkanObjectDeleter>& candidate) {
					return candidate.get() == commandList;
				});
			consumed.push_back(std::move(*acquired));
			m_acquiredCommandLists.erase(acquired);
		}
		if (m_context.device == VK_NULL_HANDLE || m_submissionFaulted) return false;

		VulkanTexture* activeBackBuffer = nullptr;
		if (m_frameReady && m_imageAcquired && m_currentImageIndex < m_backBuffers.size())
		{
			activeBackBuffer = m_backBuffers[m_currentImageIndex].get();
		}
		bool frameSubmission = false;
		uint32_t firstSwapchainCommand=count;
        VkPipelineStageFlags acquireWaitStage=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		for (uint32_t i=0;i<count;++i)
		{
			auto* commandList=selected[i];
			if (!commandList->IsValid()) return false;
			for (VulkanTexture* image : commandList->GetReferencedSwapchainImages())
			{
				if (activeBackBuffer == nullptr || image != activeBackBuffer) return false;
                const bool firstReference=!frameSubmission;
                if(firstReference)firstSwapchainCommand=i;
                if(!m_presentPending && acquireWaitStage!=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)
                    acquireWaitStage=commandList->GetSwapchainAcquireWaitStage(image,firstReference);
				frameSubmission = true;
			}
		}
		if (frameSubmission && (!m_frameReady || !m_imageAcquired ||
			m_currentFrameSlot >= m_frameSlots.size() || m_frameSlots[m_currentFrameSlot] != VK_NULL_HANDLE ||
			m_currentImageIndex >= m_renderFinishedSemaphores.size() ||
			m_currentImageIndex >= m_imagesInFlight.size()))
		{
			return false;
		}

		VulkanSubmissionState resourceStates{};
		for (VulkanCommandList* commandList : selected)
		{
			if (!commandList->ValidateForSubmit(resourceStates)) return false;
		}
        // 중간 제출의 최종 상태는 제한하지 않는다. Present에서 공통 RHI가 검사한다.

		std::vector<VkCommandBuffer> nativeCommandBuffers;
		nativeCommandBuffers.reserve(count);
		for (VulkanCommandList* commandList : selected)
		{
			nativeCommandBuffers.push_back(commandList->GetCommandBuffer());
		}

		SubmissionRecord submission{};
		submission.commandLists = std::move(consumed);
		m_submissions.reserve(m_submissions.size() + 1);
            const auto fenceCreated=AcquireSubmissionFence(submission.fence);
            if(fenceCreated!=VK_SUCCESS) {LogVulkanFailure("AcquireSubmissionFence",fenceCreated);return false;}

		const VkPipelineStageFlags waitStage = acquireWaitStage;
		VkSubmitInfo submitInfos[2]{};
		submitInfos[0].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfos[0].commandBufferCount = count;
		submitInfos[0].pCommandBuffers = nativeCommandBuffers.data();
        uint32_t submitCount=1;
		const bool waitsForAcquire = frameSubmission && !m_presentPending;
		if (waitsForAcquire)
		{
            auto* imageSubmit=&submitInfos[0];
            if(firstSwapchainCommand>0)
            {
                // Prefix command lists never touch the acquired image. Keep
                // their work and timestamps outside the semaphore wait batch.
                submitInfos[1]=submitInfos[0];
                submitInfos[0].commandBufferCount=firstSwapchainCommand;
                imageSubmit=&submitInfos[1];submitCount=2;
                imageSubmit->commandBufferCount=count-firstSwapchainCommand;
                imageSubmit->pCommandBuffers=nativeCommandBuffers.data()+firstSwapchainCommand;
            }
			imageSubmit->waitSemaphoreCount = 1;
			imageSubmit->pWaitSemaphores = &m_imageAvailableSemaphores[m_currentFrameSlot];
			imageSubmit->pWaitDstStageMask = &waitStage;
            // 표시 신호는 Present의 마지막 제출에서만 보낸다.
		}

#ifdef DY_PROFILER_TRACING_ENABLED
        Platform::CpuTimer queueSubmitTimer;
#endif
		const VkResult result = vkQueueSubmit(m_context.graphicsQueue, submitCount, submitInfos, submission.fence);
#ifdef DY_PROFILER_TRACING_ENABLED
        Platform::PublishValue("Vulkan queue submit ms",queueSubmitTimer.ElapsedMilliseconds(),true);
#endif
		if (result != VK_SUCCESS)
		{
            m_submissionFaulted = true;
            if(m_commandPools)m_commandPools->DisableReuse();
            // Keep an uncertain submission alive until shutdown drains the device.
            m_submissions.push_back(std::move(submission));
			LogVulkanFailure("vkQueueSubmit", result);
			return false;
		}
		for (VulkanCommandList* commandList : selected) commandList->CommitResourceStates();

		if (frameSubmission)
		{
            // 획득 세마포어는 첫 이미지 제출에서만 소비한다. 프레임 종료는 Present가 맡는다.
            m_presentPending = true;
		}

		submission.serial=++m_lastSubmission;
		if (waitsForAcquire && m_retiredReleaseAcquired && !m_retiredReleaseSubmission)
			m_retiredReleaseSubmission = submission.serial;
        m_submissions.push_back(std::move(submission));
		return true;
	}

    bool VulkanDevice::Impl::Present()
    {
        if(!m_frameReady || !m_imageAcquired || m_swapchain.GetHandle()==VK_NULL_HANDLE || m_submissionFaulted)return false;
        if(!PollPresentJobs(true))return false;
        auto job=std::make_unique<PresentJob>();
        job->swapchain=m_swapchain.GetHandle();job->imageIndex=m_currentImageIndex;
        job->waitSemaphore=m_renderFinishedSemaphores[m_currentImageIndex];
        job->acquireSuboptimal=m_recreateAfterPresent;
        auto* presentJob=job.get();
        struct PendingJob
        {
            std::deque<std::unique_ptr<PresentJob>>& jobs;
            bool pending=false;
            ~PendingJob() {if(pending)jobs.pop_back();}
        } pendingJob{m_presentJobs};
        if(m_asyncPresent)
        {m_presentJobs.push_back(std::move(job));pendingJob.pending=true;}
        // 모든 이미지 작업 뒤에서 한 번만 표시 세마포어를 신호한다.
        SubmissionRecord completion{};
        m_submissions.reserve(m_submissions.size()+1);
        const VkResult fenceCreated=AcquireSubmissionFence(completion.fence);
        if(fenceCreated!=VK_SUCCESS)
        {
            m_submissionFaulted=true;
            LogVulkanFailure("AcquireSubmissionFence(Present)",fenceCreated);
            return false;
        }
        const VkPipelineStageFlags waitStage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit{};
        submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;
        const bool waitsForAcquire = !m_presentPending;
        if(waitsForAcquire)
        {
            submit.waitSemaphoreCount=1;
            submit.pWaitSemaphores=&m_imageAvailableSemaphores[m_currentFrameSlot];
            submit.pWaitDstStageMask=&waitStage;
        }
        submit.signalSemaphoreCount=1;
        submit.pSignalSemaphores=&m_renderFinishedSemaphores[m_currentImageIndex];
#ifdef DY_PROFILER_TRACING_ENABLED
        Platform::CpuTimer presentSubmitTimer;
#endif
        const VkResult submitted=vkQueueSubmit(m_context.graphicsQueue,1,&submit,completion.fence);
#ifdef DY_PROFILER_TRACING_ENABLED
        const double presentSubmitMilliseconds=presentSubmitTimer.ElapsedMilliseconds();
#endif
        if(submitted!=VK_SUCCESS)
        {
            m_submissionFaulted=true;
            m_submissions.push_back(std::move(completion));
            LogVulkanFailure("vkQueueSubmit(Present)",submitted);
            return false;
        }
        completion.serial=++m_lastSubmission;
		if (waitsForAcquire && m_retiredReleaseAcquired && !m_retiredReleaseSubmission)
			m_retiredReleaseSubmission = completion.serial;
        completion.frameSlot=m_currentFrameSlot;
        completion.imageIndex=m_currentImageIndex;
        m_frameSlots[m_currentFrameSlot]=completion.fence;
        m_imagesInFlight[m_currentImageIndex]=completion.fence;
        m_submissions.push_back(std::move(completion));

        if(m_presentFencesEnabled)
        {
            const auto reset=vkResetFences(m_context.device,1,&m_presentFences[m_currentImageIndex]);
            if(reset!=VK_SUCCESS)
            {
                m_submissionFaulted=true;
                LogVulkanFailure("vkResetFences(presentation)",reset);
                return false;
            }
            presentJob->fence=m_presentFences[m_currentImageIndex];
            m_presentFencePending[m_currentImageIndex]=1;
        }
        // The previous host Present has returned. Acquire the next frame before
        // handing this swapchain to the worker, so WSI host accesses never overlap.
        TryPrefetchImage();
        m_swapchainPresented=true;
        if(m_asyncPresent)
        {
            if(!m_presentQueue->TryPush(presentJob))
            {
                if(presentJob->fence!=VK_NULL_HANDLE)m_presentFencePending[presentJob->imageIndex]=0;
                m_submissionFaulted=true;return false;
            }
            pendingJob.pending=false;
            WakePresentThread(m_presentSleeping,m_presentWake);
        }
        else CallPresent(*presentJob);
        m_presentPending=false;
        m_frameReady=false;
        m_imageAcquired=false;
        m_currentFrameSlot=(m_currentFrameSlot+1)%m_owner.GetDesc().maxFramesInFlight;
        m_recreateAfterPresent=false;
        const bool presented=m_asyncPresent || ApplyPresentResult(*presentJob);
#ifdef DY_PROFILER_TRACING_ENABLED
        // Publish only after the job and all frame state are externally observable.
        Platform::PublishValue("Vulkan present submit ms",presentSubmitMilliseconds,true);
#endif
        if(!m_asyncPresent)PublishPresentMetrics(*presentJob);
        return presented;
    }

    void VulkanDevice::Impl::PublishPresentMetrics(const PresentJob& job)
    {
#ifdef DY_PROFILER_TRACING_ENABLED
        // External trace sinks stay on the calling thread, including async results.
        Platform::PublishValue("Vulkan queue present ms",job.wallMilliseconds,true);
        if(job.cpuMilliseconds>=0)Platform::PublishValue("Vulkan queue present CPU ms",job.cpuMilliseconds,true);
#endif
        (void)job;
    }

    bool VulkanDevice::Impl::ApplyPresentResult(const PresentJob& job)
    {
        const auto result=job.result;
        if(job.swapchain!=m_swapchain.GetHandle()) {m_submissionFaulted=true;return false;}
        // OOM leaves the presentation unqueued, so its reset fence cannot signal.
        if(job.fence!=VK_NULL_HANDLE && (result==VK_ERROR_OUT_OF_HOST_MEMORY || result==VK_ERROR_OUT_OF_DEVICE_MEMORY))
            m_presentFencePending[job.imageIndex]=0;
        if((result==VK_SUCCESS || result==VK_SUBOPTIMAL_KHR) && !m_retiredSwapchains.empty() &&
            m_retiredReleaseImage==UINT32_MAX)m_retiredReleaseImage=job.imageIndex;
        if(result==VK_SUCCESS || result==VK_SUBOPTIMAL_KHR || result==VK_ERROR_OUT_OF_DATE_KHR)
        {
            if(result==VK_ERROR_OUT_OF_DATE_KHR) m_swapchainNeedsRecreate=true;
            else if(result==VK_SUBOPTIMAL_KHR || job.acquireSuboptimal)
            {
                // SUBOPTIMAL images remain usable. An unchanged advisory must not
                // create an endless chain of swapchains awaiting presentation completion.
                // Explicit requested configuration changes use CreateSwapchain instead.
                bool changed=false;
                VulkanSwapchain::InitializationStatus status;
                if(!m_swapchain.IsSurfaceConfigurationChanged(m_context,m_windowHandle,changed,status))
                {
                    m_swapchainNeedsRecreate=true;
                    if(!status.retryLater) return ReportSwapchainFailure(status);
                }
                else m_swapchainNeedsRecreate=changed && (m_presentFencesEnabled || m_retiredSwapchains.empty());
            }
            // 크기 변경으로 표시가 생략되어도 프레임은 정상 종료되었다.
            // 다음 BeginFrame이 재생성을 진행하며 준비되지 않은 동안은 프레임을 건너뛴다.
            return true;
        }
        // acquire의 SUBOPTIMAL 신호가 뒤따른 present의 실제 실패를 가리지 않게 한다.
        if(result==VK_ERROR_DEVICE_LOST)m_submissionFaulted=true;
        else m_swapchainNeedsRecreate=true;
        LogVulkanFailure("vkQueuePresentKHR",result);
        return false;
    }

	bool VulkanDevice::ReadTextureNative(dyf::RHI::TextureHandle texture, dyf::RHI::TextureReadback& result)
	{
		return m_impl->ReadTexture(texture, result);
	}

	bool VulkanDevice::Impl::ReadTexture(dyf::RHI::TextureHandle texture, dyf::RHI::TextureReadback& result)
	{
		if(texture == nullptr || m_submissionFaulted || !m_acquiredCommandLists.empty()) return false;
		const bool backBuffer = texture == m_backBuffer;
		if(backBuffer)
		{
			if(!m_swapchainDesc.allowReadback || !m_presentPending) return false;
		}
		else if(std::find(m_textures.begin(), m_textures.end(), texture) == m_textures.end()) return false;
		const auto& desc = texture->GetDesc();
		if(!dyf::RHI::IsReadbackFormat(desc.format) || !desc.width || !desc.height ||
			desc.width > UINT32_MAX / 4u) return false;
		const uint64_t bytes = static_cast<uint64_t>(desc.width) * desc.height * 4u;
		if(bytes > std::numeric_limits<size_t>::max()) return false;
		auto* image = static_cast<VulkanTexture*>(texture);
		const auto state = image->GetState(0, 0);
		if(state == dyf::RHI::ResourceState::Undefined) return false;

		auto readback = std::make_unique<ReadbackResources>();
		auto& resources = *readback;
		resources.device = m_context.device;
		resources.source = image;
		VkBufferCreateInfo bufferInfo{};
		bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferInfo.size = bytes;
		bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if(vkCreateBuffer(m_context.device, &bufferInfo, nullptr, &resources.buffer) != VK_SUCCESS) return false;
		VkMemoryRequirements requirements{};
		vkGetBufferMemoryRequirements(m_context.device, resources.buffer, &requirements);
		VkPhysicalDeviceMemoryProperties properties{};
		vkGetPhysicalDeviceMemoryProperties(m_context.physicalDevice, &properties);
		uint32_t memoryType = UINT32_MAX;
		for(uint32_t i = 0; i < properties.memoryTypeCount; ++i)
		{
			if((requirements.memoryTypeBits & (1u << i)) &&
				(properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
			{
				memoryType = i;
				if(properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) break;
			}
		}
		if(memoryType == UINT32_MAX) return false;
		VkMemoryAllocateInfo allocation{};
		allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocation.allocationSize = requirements.size;
		allocation.memoryTypeIndex = memoryType;
		if(vkAllocateMemory(m_context.device, &allocation, nullptr, &resources.memory) != VK_SUCCESS ||
			vkBindBufferMemory(m_context.device, resources.buffer, resources.memory, 0) != VK_SUCCESS) return false;
		VkCommandPoolCreateInfo poolInfo{};
		poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolInfo.queueFamilyIndex = m_context.queueIndices.graphicsFamily;
		poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
		if(vkCreateCommandPool(m_context.device, &poolInfo, nullptr, &resources.pool) != VK_SUCCESS) return false;
		VkCommandBufferAllocateInfo commandInfo{};
		commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		commandInfo.commandPool = resources.pool;
		commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		commandInfo.commandBufferCount = 1;
		VkCommandBuffer command = VK_NULL_HANDLE;
		if(vkAllocateCommandBuffers(m_context.device, &commandInfo, &command) != VK_SUCCESS) return false;
		VkCommandBufferBeginInfo begin{};
		begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		if(vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) return false;
		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barrier.oldLayout = ToVulkanImageLayout(state);
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image->GetImage();
		barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			0, 0, nullptr, 0, nullptr, 1, &barrier);
		VkBufferImageCopy copy{};
		copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copy.imageExtent = {desc.width, desc.height, 1};
		{
			dyf::RHI::LiveRhiScope observed(dyf::Platform::LiveEventKind::CpuWork,dyf::Platform::LiveStage::NativeTranslate,"Readback: record texture copy");
			observed.event.resource=dyf::Platform::FindLiveObject(texture);observed.event.bytes=bytes;observed.event.operations=1;
			vkCmdCopyImageToBuffer(command, image->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,resources.buffer,1,&copy);
		}
		VkBufferMemoryBarrier hostRead{};
		hostRead.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		hostRead.srcQueueFamilyIndex = hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		hostRead.buffer = resources.buffer;
		hostRead.size = VK_WHOLE_SIZE;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
			0, 0, nullptr, 1, &hostRead, 0, nullptr);
		std::swap(barrier.oldLayout, barrier.newLayout);
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			0, 0, nullptr, 0, nullptr, 1, &barrier);
		if(vkEndCommandBuffer(command) != VK_SUCCESS) return false;
		// This diagnostic is deliberately synchronous. It does not consume the
		// presentation semaphore or change the application's tracked resource state.
		VkSubmitInfo submit{};
		submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &command;
		const auto liveSubmission=dyf::Platform::IsLiveCollectorEnabled()?dyf::Platform::AllocateLiveId():0;
		VkResult submitted;
		{
			dyf::RHI::LiveRhiScope observed(dyf::Platform::LiveEventKind::CpuWork,dyf::Platform::LiveStage::Submit,"Readback: native submission");
			observed.event.resource=dyf::Platform::FindLiveObject(texture);observed.event.submission=liveSubmission;observed.event.operations=1;
			submitted=vkQueueSubmit(m_context.graphicsQueue,1,&submit,VK_NULL_HANDLE);
			observed.event.outcome=submitted==VK_SUCCESS?dyf::Platform::LiveOutcome::Submitted:dyf::Platform::LiveOutcome::Failed;
		}
		if(submitted != VK_SUCCESS)
		{
			// OOM submit failures leave the commands unsubmitted. Other failures
			// do not provide that guarantee, so use the shutdown retention path.
			if(submitted != VK_ERROR_OUT_OF_HOST_MEMORY && submitted != VK_ERROR_OUT_OF_DEVICE_MEMORY)
			{
				m_submissionFaulted = true;
				m_failedReadback = std::move(readback);
			}
			LogVulkanFailure("vkQueueSubmit(readback)", submitted);
			return false;
		}
		if(liveSubmission)dyf::Platform::SetLiveGpuCoverage(dyf::Platform::GetLiveContext().frame,false);
		dyf::RHI::LiveRhiScope observedWait(dyf::Platform::LiveEventKind::Wait,dyf::Platform::LiveStage::Submit,"Readback: queue wait");
		observedWait.event.resource=dyf::Platform::FindLiveObject(texture);observedWait.event.submission=liveSubmission;observedWait.event.operations=1;
		const VkResult waited = vkQueueWaitIdle(m_context.graphicsQueue);
		observedWait.event.outcome=waited==VK_SUCCESS?dyf::Platform::LiveOutcome::Completed:dyf::Platform::LiveOutcome::Failed;
		observedWait.Finish();
		if(waited != VK_SUCCESS)
		{
			m_submissionFaulted = true;
			m_failedReadback = std::move(readback);
			// A log callback may reenter the device, so publish failure ownership first.
			LogVulkanFailure("vkQueueWaitIdle(readback)", waited);
			return false;
		}
		dyf::RHI::TextureReadback output;
		dyf::RHI::RecordLiveReadbackCompletion(liveSubmission,dyf::Platform::FindLiveObject(texture));
		output.width = desc.width; output.height = desc.height;
		output.rowPitch = desc.width * 4u; output.format = desc.format;
		output.pixels.resize(static_cast<size_t>(bytes));
		void* mapped = nullptr;
		if(vkMapMemory(m_context.device, resources.memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) return false;
		if(!(properties.memoryTypes[memoryType].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
		{
			VkMappedMemoryRange range{};
			range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
			range.memory = resources.memory;
			range.size = VK_WHOLE_SIZE;
			if(vkInvalidateMappedMemoryRanges(m_context.device, 1, &range) != VK_SUCCESS)
			{
				vkUnmapMemory(m_context.device, resources.memory);
				return false;
			}
		}
		std::memcpy(output.pixels.data(), mapped, output.pixels.size());
		vkUnmapMemory(m_context.device, resources.memory);
		result = std::move(output);
		return true;
	}

	dyf::RHI::BufferHandle VulkanDevice::Impl::CreateBuffer(const dyf::RHI::BufferDesc& desc)
	{
		try
		{
			std::unique_ptr<VulkanBuffer, VulkanObjectDeleter> buffer(
				new VulkanBuffer(m_context, desc));
			VulkanBuffer* result = buffer.get();
			m_buffers.push_back(result);
			buffer.release();
			return result;
		}
		catch (const std::exception& exception)
		{
			std::fprintf(stderr, "[Vulkan] Vulkan buffer creation failed: %s\n", exception.what());
			return nullptr;
		}
	}

	dyf::RHI::TextureHandle VulkanDevice::Impl::CreateTexture(const dyf::RHI::TextureDesc& desc)
	{
		try
		{
			std::unique_ptr<VulkanTexture, VulkanObjectDeleter> texture(
				new VulkanTexture(m_context, desc));
			VulkanTexture* result = texture.get();
			m_textures.push_back(result);
			texture.release();
			return result;
		}
		catch (const std::exception& exception)
		{
			std::fprintf(stderr, "[Vulkan] Vulkan texture creation failed: %s\n", exception.what());
			return nullptr;
		}
	}

	dyf::RHI::ShaderHandle VulkanDevice::Impl::CreateShader(const dyf::RHI::ShaderDesc& desc)
	{
		try
		{
			std::unique_ptr<VulkanShader, VulkanObjectDeleter> shader(
				new VulkanShader(m_context, desc));
			VulkanShader* result = shader.get();
			m_shaders.push_back(result);
			shader.release();
			return result;
		}
		catch (const std::exception& exception)
		{
			std::fprintf(stderr, "[Vulkan] Vulkan shader creation failed: %s\n", exception.what());
			return nullptr;
		}
	}

	dyf::RHI::PipelineHandle VulkanDevice::Impl::CreateGraphicsPipeline(const dyf::RHI::GraphicsPipelineDesc& desc)
	{
		if (std::find(m_shaders.begin(), m_shaders.end(), desc.vertexShader) == m_shaders.end() ||
			(desc.hullShader != nullptr && std::find(m_shaders.begin(), m_shaders.end(), desc.hullShader) == m_shaders.end()) ||
			(desc.domainShader != nullptr && std::find(m_shaders.begin(), m_shaders.end(), desc.domainShader) == m_shaders.end()) ||
			(desc.fragmentShader != nullptr && std::find(m_shaders.begin(), m_shaders.end(), desc.fragmentShader) == m_shaders.end()))
		{
			return nullptr;
		}
		try
		{
			std::unique_ptr<VulkanPipeline, VulkanObjectDeleter> pipeline(
				new VulkanPipeline(m_context, desc));
			VulkanPipeline* result = pipeline.get();
			m_pipelines.push_back(result);
			pipeline.release();
			return result;
		}
		catch (const std::exception& exception)
		{
			std::fprintf(stderr, "[Vulkan] Vulkan pipeline creation failed: %s\n", exception.what());
			return nullptr;
		}
	}

	dyf::RHI::ResourceSetHandle VulkanDevice::Impl::CreateResourceSet(const dyf::RHI::ResourceSetDesc& desc)
	{
		if (std::find(m_pipelines.begin(), m_pipelines.end(), desc.pipeline) == m_pipelines.end() ||
			(desc.bindingCount > 0 && desc.bindings == nullptr))
		{
			return nullptr;
		}
		for (uint32_t i = 0; i < desc.bindingCount; ++i)
		{
			const dyf::RHI::ResourceBinding& binding = desc.bindings[i];
			if ((binding.buffer != nullptr && std::find(m_buffers.begin(), m_buffers.end(), binding.buffer) == m_buffers.end()) ||
				(binding.texture != nullptr && std::find(m_textures.begin(), m_textures.end(), binding.texture) == m_textures.end()))
			{
				return nullptr;
			}
		}
		try
		{
			std::unique_ptr<VulkanResourceSet, VulkanObjectDeleter> resourceSet(
				new VulkanResourceSet(m_context, desc));
			VulkanResourceSet* result = resourceSet.get();
			m_resourceSets.push_back(result);
			resourceSet.release();
			return result;
		}
		catch (const std::exception& exception)
		{
			std::fprintf(stderr, "[Vulkan] Vulkan resource-set creation failed: %s\n", exception.what());
			return nullptr;
		}
	}

	void VulkanDevice::Impl::DestroyBuffer(dyf::RHI::BufferHandle buffer)
	{
		const auto found = std::find(m_buffers.begin(), m_buffers.end(), buffer);
		if(found == m_buffers.end()) return;
		m_buffers.erase(found);
		VulkanObjectDeleter{}(static_cast<VulkanBuffer*>(buffer));
    }

	void VulkanDevice::Impl::DestroyTexture(dyf::RHI::TextureHandle texture)
	{
		const auto found = std::find(m_textures.begin(), m_textures.end(), texture);
		if(found == m_textures.end()) return;
		m_textures.erase(found);
		auto* native = static_cast<VulkanTexture*>(texture);
		if(m_failedReadback && m_failedReadback->source == native)
			m_failedReadback->retiredSource.reset(native);
		else
			VulkanObjectDeleter{}(native);
    }

	void VulkanDevice::Impl::DestroyShader(dyf::RHI::ShaderHandle shader)
	{
		const auto found = std::find(m_shaders.begin(), m_shaders.end(), shader);
		if(found == m_shaders.end()) return;
		m_shaders.erase(found);
		VulkanObjectDeleter{}(static_cast<VulkanShader*>(shader));
    }

	void VulkanDevice::Impl::DestroyPipeline(dyf::RHI::PipelineHandle pipeline)
	{
		const auto found = std::find(m_pipelines.begin(), m_pipelines.end(), pipeline);
		if(found == m_pipelines.end()) return;
		m_pipelines.erase(found);
		VulkanObjectDeleter{}(static_cast<VulkanPipeline*>(pipeline));
    }

	void VulkanDevice::Impl::DestroyResourceSet(dyf::RHI::ResourceSetHandle resourceSet)
	{
		const auto found = std::find(m_resourceSets.begin(), m_resourceSets.end(), resourceSet);
		if(found == m_resourceSets.end()) return;
		m_resourceSets.erase(found);
		VulkanObjectDeleter{}(static_cast<VulkanResourceSet*>(resourceSet));
    }

	bool VulkanDevice::Impl::UpdateBuffer(
		dyf::RHI::ICommandList& commandList,
		dyf::RHI::BufferHandle buffer,
		uint32_t offset,
		const void* data,
		uint32_t size)
	{
		VulkanCommandList* nativeCommandList = dynamic_cast<VulkanCommandList*>(&commandList);
		VulkanBuffer* nativeBuffer = dynamic_cast<VulkanBuffer*>(buffer);
		if (nativeCommandList == nullptr || nativeBuffer == nullptr ||
			std::find(m_buffers.begin(), m_buffers.end(), buffer) == m_buffers.end() ||
			std::none_of(m_acquiredCommandLists.begin(), m_acquiredCommandLists.end(),
				[nativeCommandList](
					const std::unique_ptr<VulkanCommandList, VulkanObjectDeleter>& candidate) {
					return candidate.get() == nativeCommandList;
				}))
		{
			return false;
		}
		return nativeCommandList->RecordBufferUpdate(*nativeBuffer, offset, data, size);
	}

	bool VulkanDevice::Impl::UpdateTexture(
		dyf::RHI::ICommandList& commandList,
		dyf::RHI::TextureHandle texture,
		uint32_t mipLevel,
		uint32_t arrayLayer,
		const void* data,
		uint32_t dataSize,
		uint32_t rowPitch,
		uint32_t slicePitch)
	{
		VulkanCommandList* nativeCommandList = dynamic_cast<VulkanCommandList*>(&commandList);
		VulkanTexture* nativeTexture = dynamic_cast<VulkanTexture*>(texture);
		if (nativeCommandList == nullptr || nativeTexture == nullptr ||
			std::find(m_textures.begin(), m_textures.end(), texture) == m_textures.end() ||
			std::none_of(m_acquiredCommandLists.begin(), m_acquiredCommandLists.end(),
				[nativeCommandList](
					const std::unique_ptr<VulkanCommandList, VulkanObjectDeleter>& candidate) {
					return candidate.get() == nativeCommandList;
				}))
		{
			return false;
		}
		return nativeCommandList->RecordTextureUpdate(
			*nativeTexture,
			mipLevel,
			arrayLayer,
			data,
			dataSize,
			rowPitch,
			slicePitch);
	}

	bool VulkanDevice::Impl::CreateInstance()
	{
        const auto enumerateVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
        if(enumerateVersion && enumerateVersion(&m_context.apiVersion)!=VK_SUCCESS)return false;
        m_context.apiVersion=std::min(m_context.apiVersion,uint32_t(VK_API_VERSION_1_3));
        uint32_t availableCount=0;
        if(vkEnumerateInstanceExtensionProperties(nullptr,&availableCount,nullptr)!=VK_SUCCESS)return false;
        std::vector<VkExtensionProperties> available(availableCount);
        if(vkEnumerateInstanceExtensionProperties(nullptr,&availableCount,available.data())!=VK_SUCCESS)return false;
        const auto supported=[&](const char* name) {
            return std::any_of(available.begin(),available.end(),[&](const auto& e){return std::strcmp(e.extensionName,name)==0;});
        };
        std::vector<const char*> extensions;
        m_context.debugUtilsEnabled=supported(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        if(m_context.debugUtilsEnabled)extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        if(supported(VK_KHR_SURFACE_EXTENSION_NAME))extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
#if defined(_WIN32)
        if(supported("VK_KHR_win32_surface"))extensions.push_back("VK_KHR_win32_surface");
#elif defined(__ANDROID__)
        if(supported("VK_KHR_android_surface"))extensions.push_back("VK_KHR_android_surface");
#else
        for(const char* name:{"VK_KHR_xlib_surface","VK_KHR_xcb_surface","VK_KHR_wayland_surface"})
            if(supported(name))extensions.push_back(name);
#endif
        if(supported(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME))
            extensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
        m_surfaceMaintenanceEnabled=m_context.apiVersion>=VK_API_VERSION_1_1 && supported(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME) &&
            supported(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
        if(m_surfaceMaintenanceEnabled)
        {
            extensions.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
            extensions.push_back(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
        }
        const bool captureValidation = m_owner.GetDesc().enableValidation && m_context.debugUtilsEnabled;
        if (m_owner.GetDesc().enableValidation && !captureValidation)
            std::fprintf(stderr, "[Vulkan] Validation log capture unavailable: VK_EXT_debug_utils is missing\n");
        VkDebugUtilsMessengerCreateInfoEXT debugInfo{};
        debugInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        debugInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debugInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debugInfo.pfnUserCallback = ValidationMessage;
        const uint32_t extensionCount=static_cast<uint32_t>(extensions.size());

		std::vector<const char*> layers;
		if(m_owner.GetDesc().enableValidation) {if(!ValidationLayerAvailable())return false;layers.push_back(kValidationLayerName);}

		VkApplicationInfo application{};
		application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		application.pApplicationName = "dy_engine";
		application.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
		application.pEngineName = "dy_engine";
		application.engineVersion = VK_MAKE_VERSION(1, 0, 0);
		application.apiVersion = m_context.apiVersion;

		VkInstanceCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        info.pNext = captureValidation ? &debugInfo : nullptr;
		info.pApplicationInfo = &application;
		info.enabledExtensionCount = extensionCount;
		info.ppEnabledExtensionNames = extensions.data();
		info.enabledLayerCount = static_cast<uint32_t>(layers.size());
		info.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();
		const VkResult result = vkCreateInstance(&info, nullptr, &m_context.instance);
		if (result != VK_SUCCESS) LogVulkanFailure("vkCreateInstance", result);
        if (result == VK_SUCCESS && captureValidation)
        {
            const auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(m_context.instance, "vkCreateDebugUtilsMessengerEXT"));
            if (createMessenger)
            {
                const auto captureResult = createMessenger(m_context.instance, &debugInfo, nullptr, &m_debugMessenger);
                if (captureResult != VK_SUCCESS) LogVulkanFailure("vkCreateDebugUtilsMessengerEXT", captureResult);
            }
            if (m_debugMessenger == VK_NULL_HANDLE)
                std::fprintf(stderr, "[Vulkan] Validation log capture is not active\n");
        }
		return result == VK_SUCCESS;
	}

	VkResult VulkanDevice::Impl::CreateSurface()
	{
#if defined(_WIN32)
		VkWin32SurfaceCreateInfoKHR info{};
		info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
		info.hinstance = GetModuleHandle(nullptr);
		info.hwnd = static_cast<HWND>(m_windowHandle);
		return vkCreateWin32SurfaceKHR(m_context.instance, &info, nullptr, &m_context.surface);
#elif defined(__ANDROID__)
		VkAndroidSurfaceCreateInfoKHR info{};
		info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
		info.window = static_cast<ANativeWindow*>(m_windowHandle);
		return vkCreateAndroidSurfaceKHR(m_context.instance, &info, nullptr, &m_context.surface);
#else
		return glfwCreateWindowSurface(
			m_context.instance,
			static_cast<GLFWwindow*>(m_windowHandle),
			nullptr,
			&m_context.surface);
#endif
	}

	bool VulkanDevice::Impl::PickPhysicalDevice()
	{
		uint32_t deviceCount = 0;
		if (vkEnumeratePhysicalDevices(m_context.instance, &deviceCount, nullptr) != VK_SUCCESS || deviceCount == 0) return false;
		std::vector<VkPhysicalDevice> devices(deviceCount);
		if (vkEnumeratePhysicalDevices(m_context.instance, &deviceCount, devices.data()) != VK_SUCCESS) return false;

		if(m_owner.GetDesc().adapterIndex>=deviceCount)return false;
		const VkPhysicalDevice device = devices[m_owner.GetDesc().adapterIndex];
		VkPhysicalDeviceProperties properties{};
		vkGetPhysicalDeviceProperties(device, &properties);
		m_context.apiVersion = std::min(m_context.apiVersion, properties.apiVersion);
        m_context.negativeViewportSupported=m_context.apiVersion>=VK_API_VERSION_1_1;

		VkPhysicalDeviceVulkan13Features vulkan13{};
		vulkan13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
		VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceFeatures(device,&features);
        if(m_context.apiVersion>=VK_API_VERSION_1_3)
        {
            const auto getFeatures2=reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
                vkGetInstanceProcAddr(m_context.instance,"vkGetPhysicalDeviceFeatures2"));
            VkPhysicalDeviceFeatures2 features2{};
            features2.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;features2.pNext=&vulkan13;
            if(getFeatures2)getFeatures2(device,&features2);
            m_context.modernRendering=vulkan13.dynamicRendering && vulkan13.synchronization2;
        }

		uint32_t familyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
		std::vector<VkQueueFamilyProperties> families(familyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());
		VulkanContext::QueueFamilyIndices indices{};
		for (uint32_t i = 0; i < familyCount; ++i)
		{
			if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) indices.graphicsFamily = i;
			if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) indices.presentFamily = i;
			if (indices.IsComplete()) break;
		}

		if (!indices.IsComplete()) return false;

            std::fprintf(stderr, "[Vulkan] GPU=%s vendor=%u device=%u driverVersionRaw=%u api=%u.%u.%u path=%s validation=%s capture=%s\n",
                properties.deviceName, properties.vendorID, properties.deviceID, properties.driverVersion,
                VK_API_VERSION_MAJOR(m_context.apiVersion), VK_API_VERSION_MINOR(m_context.apiVersion), VK_API_VERSION_PATCH(m_context.apiVersion),
                m_context.modernRendering ? "modern" : "legacy",
                m_owner.GetDesc().enableValidation ? "on" : "off", m_debugMessenger != VK_NULL_HANDLE ? "on" : "off");
		m_context.physicalDevice = device;
		m_context.queueIndices = indices;
		// 선택 시 조회한 불변 정보는 모든 명령 래퍼와 기능 검사에서 함께 사용한다.
		auto physicalInfo=std::make_shared<VulkanPhysicalDeviceInfo>();
		physicalInfo->properties=properties;
		physicalInfo->features=features;
		physicalInfo->graphicsQueue=families[indices.graphicsFamily];
        physicalInfo->maintenance4Supported=vulkan13.maintenance4==VK_TRUE;
		m_context.physicalInfo=std::move(physicalInfo);
		return true;
	}

	bool VulkanDevice::Impl::CreateLogicalDevice()
	{
		if(!m_context.physicalInfo) return false;
        bool separatePresent=false;
#if defined(__linux__) && !defined(__ANDROID__)
        separatePresent=m_owner.GetDesc().maxFramesInFlight>1;
#endif
        const bool sameFamily=m_context.queueIndices.graphicsFamily==m_context.queueIndices.presentFamily;
        const uint32_t presentQueueIndex=separatePresent && sameFamily &&
            m_context.physicalInfo->graphicsQueue.queueCount>1 ? 1u : 0u;
        const float priorities[]={1.0f,1.0f};
        VkDeviceQueueCreateInfo queueInfos[2]{};
        queueInfos[0].sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfos[0].queueFamilyIndex=m_context.queueIndices.graphicsFamily;
        queueInfos[0].queueCount=presentQueueIndex+1;
        queueInfos[0].pQueuePriorities=priorities;
        queueInfos[1]=queueInfos[0];
        queueInfos[1].queueFamilyIndex=m_context.queueIndices.presentFamily;
        queueInfos[1].queueCount=1;

		const auto& supported=m_context.physicalInfo->features;
		VkPhysicalDeviceFeatures enabled{};
		enabled.samplerAnisotropy = supported.samplerAnisotropy;
        enabled.textureCompressionBC = supported.textureCompressionBC;
		enabled.fillModeNonSolid = supported.fillModeNonSolid;
		enabled.depthBiasClamp = supported.depthBiasClamp;
		enabled.tessellationShader = supported.tessellationShader;
		enabled.drawIndirectFirstInstance = supported.drawIndirectFirstInstance;
        enabled.independentBlend=supported.independentBlend;
        enabled.vertexPipelineStoresAndAtomics=supported.vertexPipelineStoresAndAtomics;
        enabled.fragmentStoresAndAtomics=supported.fragmentStoresAndAtomics;
		enabled.shaderSampledImageArrayDynamicIndexing = supported.shaderSampledImageArrayDynamicIndexing;

		VkPhysicalDeviceVulkan13Features vulkan13{};
		vulkan13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
		vulkan13.dynamicRendering = VK_TRUE;
		vulkan13.synchronization2 = VK_TRUE;
		vulkan13.maintenance4 = m_context.physicalInfo->maintenance4Supported;
        void* featureChain=m_context.modernRendering ? &vulkan13 : nullptr;
        const auto getFeatures2=m_context.apiVersion>=VK_API_VERSION_1_1 ? reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            vkGetInstanceProcAddr(m_context.instance,"vkGetPhysicalDeviceFeatures2")) : nullptr;

		uint32_t extensionCount = 0;
		vkEnumerateDeviceExtensionProperties(m_context.physicalDevice, nullptr, &extensionCount, nullptr);
		std::vector<VkExtensionProperties> availableExtensions(extensionCount);
		vkEnumerateDeviceExtensionProperties(m_context.physicalDevice, nullptr, &extensionCount, availableExtensions.data());

		bool hasMeshShaderExtension = false;
		for (const auto& ext : availableExtensions)
		{
			if (std::strcmp(ext.extensionName, VK_EXT_MESH_SHADER_EXTENSION_NAME) == 0)
			{
				hasMeshShaderExtension = true;
				break;
			}
		}

		VkPhysicalDeviceMeshShaderFeaturesEXT meshFeaturesSupported{};
		meshFeaturesSupported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
		VkPhysicalDeviceMeshShaderFeaturesEXT meshFeaturesToEnable{};
		meshFeaturesToEnable.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
		bool enableMeshShader = false;

		if (m_context.modernRendering && hasMeshShaderExtension && getFeatures2)
		{
			VkPhysicalDeviceFeatures2 features2{};
			features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
			features2.pNext = &meshFeaturesSupported;
			getFeatures2(m_context.physicalDevice, &features2);
			if (meshFeaturesSupported.meshShader == VK_TRUE)
			{
				enableMeshShader = true;
				meshFeaturesToEnable.meshShader = VK_TRUE;
				meshFeaturesToEnable.taskShader = VK_FALSE;
			}
		}

		std::vector<const char*> extensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        if(!m_context.negativeViewportSupported && std::any_of(availableExtensions.begin(),availableExtensions.end(),[](const auto& extension) {
            return std::strcmp(extension.extensionName,VK_KHR_MAINTENANCE_1_EXTENSION_NAME)==0;
        }))
        {
            extensions.push_back(VK_KHR_MAINTENANCE_1_EXTENSION_NAME);
            m_context.negativeViewportSupported=true;
        }
        const char* calibrationFunction = nullptr;
#if defined(_WIN32)
        const char* calibrationExtension = nullptr;
        for(const auto& extension : availableExtensions)
        {
            if(std::strcmp(extension.extensionName, "VK_KHR_calibrated_timestamps") == 0)
            { calibrationExtension = "VK_KHR_calibrated_timestamps"; break; }
            if(std::strcmp(extension.extensionName, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) == 0)
                calibrationExtension = VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
        }
        if(m_context.apiVersion>=VK_API_VERSION_1_1 && calibrationExtension && m_owner.GetTimestampValidBits())
        {
            const bool khr = std::strcmp(calibrationExtension, "VK_KHR_calibrated_timestamps") == 0;
            const auto getDomains = reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT>(
                vkGetInstanceProcAddr(m_context.instance, khr ? "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR" : "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT"));
            uint32_t domainCount = 0;
            LARGE_INTEGER frequency;
            if(getDomains && getDomains(m_context.physicalDevice, &domainCount, nullptr) == VK_SUCCESS && domainCount &&
                QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0)
            {
                std::vector<VkTimeDomainEXT> domains(domainCount);
                if(getDomains(m_context.physicalDevice, &domainCount, domains.data()) == VK_SUCCESS &&
                    std::find(domains.begin(), domains.begin() + domainCount, VK_TIME_DOMAIN_DEVICE_EXT) != domains.begin() + domainCount &&
                    std::find(domains.begin(), domains.begin() + domainCount, VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT) != domains.begin() + domainCount)
                {
                    extensions.push_back(calibrationExtension);
                    m_context.cpuTimestampFrequency = static_cast<uint64_t>(frequency.QuadPart);
                    calibrationFunction = khr ? "vkGetCalibratedTimestampsKHR" : "vkGetCalibratedTimestampsEXT";
                }
            }
        }
#endif
		if (enableMeshShader)
		{
			extensions.push_back(VK_EXT_MESH_SHADER_EXTENSION_NAME);
			meshFeaturesToEnable.pNext = featureChain;
            featureChain=&meshFeaturesToEnable;
		}

        VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance{};
        maintenance.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT;
        if(getFeatures2 && m_surfaceMaintenanceEnabled && std::any_of(availableExtensions.begin(),availableExtensions.end(),[](const auto& extension) {
            return std::strcmp(extension.extensionName,VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)==0;
        }))
        {
            VkPhysicalDeviceFeatures2 features{};
            features.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features.pNext=&maintenance;
            getFeatures2(m_context.physicalDevice,&features);
            m_presentFencesEnabled=maintenance.swapchainMaintenance1==VK_TRUE;
            if(m_presentFencesEnabled)
            {
                extensions.push_back(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
                maintenance.pNext=featureChain;
                featureChain=&maintenance;
            }
        }

		VkDeviceCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		info.pNext = featureChain;
		info.queueCreateInfoCount = sameFamily ? 1u : 2u;
		info.pQueueCreateInfos = queueInfos;
		info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
		info.ppEnabledExtensionNames = extensions.data();
		info.pEnabledFeatures = &enabled;
		if (vkCreateDevice(m_context.physicalDevice, &info, nullptr, &m_context.device) != VK_SUCCESS) return false;
        if(m_context.modernRendering)
        {
            m_context.beginRendering=reinterpret_cast<PFN_vkCmdBeginRendering>(vkGetDeviceProcAddr(m_context.device,"vkCmdBeginRendering"));
            m_context.endRendering=reinterpret_cast<PFN_vkCmdEndRendering>(vkGetDeviceProcAddr(m_context.device,"vkCmdEndRendering"));
            m_context.pipelineBarrier2=reinterpret_cast<PFN_vkCmdPipelineBarrier2>(vkGetDeviceProcAddr(m_context.device,"vkCmdPipelineBarrier2"));
            m_context.writeTimestamp2=reinterpret_cast<PFN_vkCmdWriteTimestamp2>(vkGetDeviceProcAddr(m_context.device,"vkCmdWriteTimestamp2"));
            if(!m_context.beginRendering || !m_context.endRendering || !m_context.pipelineBarrier2 || !m_context.writeTimestamp2)return false;
        }
#ifdef DY_RENDERDOC_ENABLED
        if(m_context.debugUtilsEnabled)
        {
            m_context.beginDebugLabel=reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
                vkGetDeviceProcAddr(m_context.device,"vkCmdBeginDebugUtilsLabelEXT"));
            m_context.endDebugLabel=reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
                vkGetDeviceProcAddr(m_context.device,"vkCmdEndDebugUtilsLabelEXT"));
            m_context.insertDebugLabel=reinterpret_cast<PFN_vkCmdInsertDebugUtilsLabelEXT>(
                vkGetDeviceProcAddr(m_context.device,"vkCmdInsertDebugUtilsLabelEXT"));
        }
#endif
        std::fprintf(stderr, "[Vulkan] %s\n", m_presentFencesEnabled
            ? "Swapchain retirement: presentation fences enabled."
            : "Swapchain retirement: presentation fences unavailable; replacement waits for image reacquisition.");
		vkGetDeviceQueue(m_context.device, m_context.queueIndices.graphicsFamily, 0, &m_context.graphicsQueue);
		vkGetDeviceQueue(m_context.device, m_context.queueIndices.presentFamily, presentQueueIndex, &m_context.presentQueue);
        if(calibrationFunction)
            m_context.getCalibratedTimestamps = reinterpret_cast<PFN_vkGetCalibratedTimestampsEXT>(
                vkGetDeviceProcAddr(m_context.device, calibrationFunction));

		if (enableMeshShader)
		{
			m_context.vkCmdDrawMeshTasksEXT = reinterpret_cast<PFN_vkCmdDrawMeshTasksEXT>(
				vkGetDeviceProcAddr(m_context.device, "vkCmdDrawMeshTasksEXT"));
			m_context.meshShaderSupported = (m_context.vkCmdDrawMeshTasksEXT != nullptr);
		}
		else
		{
			m_context.meshShaderSupported = false;
			m_context.vkCmdDrawMeshTasksEXT = nullptr;
		}
		return true;
	}

	bool VulkanDevice::Impl::CreateSwapchainSyncObjects()
	{
		m_imageAvailableSemaphores.assign(m_owner.GetDesc().maxFramesInFlight, VK_NULL_HANDLE);
		m_renderFinishedSemaphores.assign(m_swapchain.GetImageCount(), VK_NULL_HANDLE);
		m_imagesInFlight.assign(m_swapchain.GetImageCount(), VK_NULL_HANDLE);
        if(m_asyncPresent)
        {
            m_acquireFences.assign(m_owner.GetDesc().maxFramesInFlight,VK_NULL_HANDLE);
            m_acquireFencePending.assign(m_acquireFences.size(),0);
            VkFenceCreateInfo fence{};fence.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fence.flags=VK_FENCE_CREATE_SIGNALED_BIT;
            for(auto& handle:m_acquireFences)
            {
                const auto result=vkCreateFence(m_context.device,&fence,nullptr,&handle);
                if(result!=VK_SUCCESS)
                {m_submissionFaulted=true;LogVulkanFailure("vkCreateFence(acquire)",result);DestroySwapchainSyncObjects();return false;}
            }
        }
		VkSemaphoreCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		const auto createSemaphores = [&](std::vector<VkSemaphore>& semaphores, const char* operation)
		{
			for (VkSemaphore& semaphore : semaphores)
			{
				const VkResult result = vkCreateSemaphore(m_context.device, &info, nullptr, &semaphore);
				if (result != VK_SUCCESS)
				{
					m_submissionFaulted = true;
					DestroySwapchainSyncObjects();
					LogVulkanFailure(operation, result);
					return false;
				}
			}
			return true;
		};
        if(!createSemaphores(m_imageAvailableSemaphores,"vkCreateSemaphore(acquire)") ||
            !createSemaphores(m_renderFinishedSemaphores,"vkCreateSemaphore(present)"))return false;
        if(m_presentFencesEnabled)
        {
            m_presentFences.assign(m_swapchain.GetImageCount(),VK_NULL_HANDLE);
            m_presentFencePending.assign(m_presentFences.size(),0);
            VkFenceCreateInfo fence{};
            fence.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fence.flags=VK_FENCE_CREATE_SIGNALED_BIT;
            for(auto& handle:m_presentFences)
            {
                const auto result=vkCreateFence(m_context.device,&fence,nullptr,&handle);
                if(result!=VK_SUCCESS)
                {
                    m_submissionFaulted=true;
                    DestroySwapchainSyncObjects();
                    LogVulkanFailure("vkCreateFence(presentation)",result);
                    return false;
                }
            }
        }
        return true;
	}

	void VulkanDevice::Impl::UpdateBackBuffers()
	{
		const std::vector<VkImage>& images = m_swapchain.GetImages();
		const std::vector<VkImageView>& views = m_swapchain.GetImageViews();
		if (images.size() != views.size() || images.empty()) throw std::runtime_error("Invalid Vulkan swapchain images");
		const VkExtent2D extent = m_swapchain.GetExtent();
		dyf::RHI::TextureDesc desc{};
		desc.width = extent.width;
		desc.height = extent.height;
		desc.depthOrArraySize = 1;
		desc.mipLevels = 1;
		desc.format = FromVulkanColorFormat(m_swapchain.GetImageFormat());
		desc.usage = dyf::RHI::TextureUsage::RenderTarget;
		if (desc.format == dyf::RHI::Format::Unknown) throw std::runtime_error("Unsupported Vulkan swapchain format");

		m_backBuffers.clear();
		m_backBuffers.reserve(images.size());
		for (size_t i = 0; i < images.size(); ++i)
		{
			m_backBuffers.push_back(
				std::unique_ptr<VulkanTexture, VulkanObjectDeleter>(
					new VulkanTexture(desc, images[i], views[i])));
		}
		m_backBuffer = m_backBuffers.front().get();
	}

    VkResult VulkanDevice::Impl::AcquireSubmissionFence(VkFence& fence)
    {
        fence=VK_NULL_HANDLE;
        if(!m_availableFences.empty())
        {
            const auto available=m_availableFences.back();m_availableFences.pop_back();
            const auto result=vkResetFences(m_context.device,1,&available);
            if(result!=VK_SUCCESS)
            {
                vkDestroyFence(m_context.device,available,nullptr);
                if(result==VK_ERROR_DEVICE_LOST)m_submissionFaulted=true;
                return result;
            }
            fence=available;return VK_SUCCESS;
        }
        VkFenceCreateInfo desc{};desc.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        const auto result=vkCreateFence(m_context.device,&desc,nullptr,&fence);
        if(result==VK_ERROR_DEVICE_LOST)m_submissionFaulted=true;
        return result;
    }

    void VulkanDevice::Impl::RecycleSubmissionFence(VkFence fence) noexcept
    {
        try
        {
            if(m_availableFences.size()<64) {m_availableFences.push_back(fence);return;}
        }
        catch(...) {} // Caching is optional; completion already made destruction safe.
        vkDestroyFence(m_context.device,fence,nullptr);
    }

	void VulkanDevice::Impl::CollectCompletedSubmissions()
	{
        if(m_presentDeviceLost.load(std::memory_order_acquire))m_submissionFaulted=true;
        if(m_commandPools && m_commandPools->IsDeviceLost())m_submissionFaulted=true;
		if (m_context.device == VK_NULL_HANDLE || m_submissionFaulted) return;
		for (size_t i = 0; i < m_submissions.size();)
		{
			SubmissionRecord& submission = m_submissions[i];
			const VkResult status = vkGetFenceStatus(m_context.device, submission.fence);
            // 완료 번호는 앞선 모든 제출까지 완료되었음을 뜻한다.
            if(status==VK_NOT_READY)break;
			if (status != VK_SUCCESS)
			{
				m_submissionFaulted = true;
                if(m_commandPools)m_commandPools->DisableReuse();
				LogVulkanFailure("vkGetFenceStatus", status);
				return;
			}

			if (submission.frameSlot < m_frameSlots.size() && m_frameSlots[submission.frameSlot] == submission.fence)
			{
				m_frameSlots[submission.frameSlot] = VK_NULL_HANDLE;
			}
			if (submission.imageIndex < m_imagesInFlight.size() && m_imagesInFlight[submission.imageIndex] == submission.fence)
			{
				m_imagesInFlight[submission.imageIndex] = VK_NULL_HANDLE;
			}
            for(const auto& commands:submission.commandLists)
            {
                commands->CompleteTimestampResets();
                commands->AllowPoolReuse();
            }
			m_completedSubmission=std::max(m_completedSubmission,submission.serial);
            RecycleSubmissionFence(submission.fence);
			m_submissions.erase(m_submissions.begin() + static_cast<std::ptrdiff_t>(i));
		}
        DestroyRetiredSwapchains(true);
		// Without presentation fences, reacquisition remains the completion proof.
		if (!m_presentFencesEnabled && !m_submissionFaulted && m_retiredReleaseSubmission &&
			m_completedSubmission >= m_retiredReleaseSubmission)
			DestroyRetiredSwapchains();
	}

	bool VulkanDevice::Impl::RecreateSwapchain()
	{
		if(!PollPresentJobs(true))return false;
		if(WaitForAcquisitions(0)!=VK_SUCCESS)return false;
        m_prefetchedImage=false;
		if (!m_hasSwapchainDesc || m_context.device == VK_NULL_HANDLE || m_submissionFaulted) return false;
		CollectCompletedSubmissions();
		if (m_submissionFaulted || m_frameReady || m_imageAcquired || m_presentPending) return false;
        // Uploads or deformation may submit before every BeginFrame. Merely
        // retrying while those submissions are pending can starve recreation.
        // Drain only this exceptional path; WaitIdle first joins host Present
        // calls before waiting for queues and retiring their command lists.
        if(!m_submissions.empty() && !WaitIdle())return false;
		if (m_swapchain.GetHandle() != VK_NULL_HANDLE)
		{
            if(!m_owner.TryBeginSwapchainRetirement())return false;
            try { m_recreationOldSwapchain = RetireSwapchainGeneration(); }
            catch(...) { m_owner.CompleteSwapchainRetirement(); throw; }
		}
		return CreateSwapchain(m_swapchainDesc, m_recreationOldSwapchain);
	}

	VkSwapchainKHR VulkanDevice::Impl::RetireSwapchainGeneration()
	{
        // Allocate the retirement record before changing any native ownership.
        auto& generation=m_retiredSwapchains.emplace_back();
        // Recreate has proved completion of both GPU submissions and acquisitions.
        for(auto fence:m_acquireFences)vkDestroyFence(m_context.device,fence,nullptr);
        m_acquireFences.clear();m_acquireFencePending.clear();
		const VkSwapchainKHR handle = m_swapchain.GetHandle();
		generation.swapchain = std::move(m_swapchain);
		generation.imageAvailableSemaphores = std::move(m_imageAvailableSemaphores);
		generation.renderFinishedSemaphores = std::move(m_renderFinishedSemaphores);
        generation.presentFences=std::move(m_presentFences);
        generation.presentFencePending=std::move(m_presentFencePending);
        generation.presented=m_swapchainPresented;
		generation.backBuffers = std::move(m_backBuffers);
		m_backBuffer = nullptr;
		m_imagesInFlight.clear();
		m_currentImageIndex = 0;
		m_recreateAfterPresent = false;
        m_swapchainPresented=false;
		return handle;
	}

	void VulkanDevice::Impl::DestroySwapchainSyncObjects()
	{
		if (m_context.device != VK_NULL_HANDLE)
		{
			for (VkSemaphore semaphore : m_imageAvailableSemaphores)
			{
				if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(m_context.device, semaphore, nullptr);
			}
			for (VkSemaphore semaphore : m_renderFinishedSemaphores)
			{
				if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(m_context.device, semaphore, nullptr);
			}
            for(VkFence fence:m_presentFences)
                if(fence!=VK_NULL_HANDLE)vkDestroyFence(m_context.device,fence,nullptr);
            for(VkFence fence:m_acquireFences)
                if(fence!=VK_NULL_HANDLE)vkDestroyFence(m_context.device,fence,nullptr);
		}
		m_imageAvailableSemaphores.clear();
		m_renderFinishedSemaphores.clear();
        m_presentFences.clear();
        m_presentFencePending.clear();
        m_acquireFences.clear();m_acquireFencePending.clear();
		m_imagesInFlight.clear();
	}

	void VulkanDevice::Impl::DestroyCurrentSwapchain()
	{
        PollPresentJobs(true);
        const auto acquired=WaitForAcquisitions();
        if(acquired!=VK_SUCCESS && acquired!=VK_ERROR_DEVICE_LOST)return;
        m_prefetchedImage=false;
		DestroySwapchainSyncObjects();
		m_backBuffer = nullptr;
		m_backBuffers.clear();
		if (m_context.device != VK_NULL_HANDLE) m_swapchain.Cleanup(m_context.device);
	}

	void VulkanDevice::Impl::DestroyRetiredSwapchains(bool completedOnly)
	{
		if (m_context.device == VK_NULL_HANDLE) return;
        for(size_t i=0;i<m_retiredSwapchains.size();)
		{
            auto& generation=m_retiredSwapchains[i];
            if(completedOnly)
            {
                // Keep the handle passed to the next vkCreateSwapchainKHR alive.
                if(generation.swapchain.GetHandle()==m_recreationOldSwapchain) {++i;continue;}
                if(generation.presented)
                {
                    if(!m_presentFencesEnabled) {++i;continue;}
                    bool ready=true;
                    for(size_t i=0;i<generation.presentFences.size();++i)
                    {
                        if(!generation.presentFencePending[i])continue;
                        const auto fence=generation.presentFences[i];
                        const auto status=vkGetFenceStatus(m_context.device,fence);
                        if(status==VK_NOT_READY) {ready=false;break;}
                        if(status!=VK_SUCCESS)
                        {
                            m_submissionFaulted=true;
                            LogVulkanFailure("vkGetFenceStatus(retired presentation)",status);
                            return;
                        }
                    }
                    if(!ready) {++i;continue;}
                }
            }
			for (VkSemaphore semaphore : generation.imageAvailableSemaphores)
			{
				if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(m_context.device, semaphore, nullptr);
			}
			for (VkSemaphore semaphore : generation.renderFinishedSemaphores)
			{
				if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(m_context.device, semaphore, nullptr);
			}
			generation.backBuffers.clear();
            for(VkFence fence:generation.presentFences)
                vkDestroyFence(m_context.device,fence,nullptr);
			generation.swapchain.Cleanup(m_context.device);
            m_retiredSwapchains.erase(m_retiredSwapchains.begin()+static_cast<std::ptrdiff_t>(i));
		}
        if(m_retiredSwapchains.empty())
        {
            m_owner.CompleteSwapchainRetirement();
            m_retiredReleaseImage = UINT32_MAX;
            m_retiredReleaseAcquired = false;
            m_retiredReleaseSubmission = 0;
        }
	}

	void VulkanDevice::Impl::DestroyDeviceResources()
	{
        if(m_commandPools)m_commandPools->DisableReuse();
		if (m_context.device != VK_NULL_HANDLE)
		{
			// 소유 장치의 소멸자가 큐 완료 또는 실제 DEVICE_LOST를 확인한 뒤에만 호출된다.
            // Public resources (including queries) were already released. Retire
            // native records without publishing query initialization during teardown.
			for (SubmissionRecord& submission : m_submissions)
			{
				if (submission.fence != VK_NULL_HANDLE) vkDestroyFence(m_context.device, submission.fence, nullptr);
			}
			m_submissions.clear();
            for(const auto fence:m_availableFences)vkDestroyFence(m_context.device,fence,nullptr);
            m_availableFences.clear();
		}
		m_acquiredCommandLists.clear();
        m_commandPools.reset();
		// Commands have retired under their fences (or shutdown established idle/loss).
		// Drop cached native pages while VkDevice still exists.
		m_uploadPool.reset();
		m_failedReadback.reset();
		std::fill(m_frameSlots.begin(), m_frameSlots.end(), VK_NULL_HANDLE);

		for (dyf::RHI::ResourceSetHandle resourceSet : m_resourceSets)
			VulkanObjectDeleter{}(static_cast<VulkanResourceSet*>(resourceSet));
		m_resourceSets.clear();
		for (dyf::RHI::PipelineHandle pipeline : m_pipelines)
			VulkanObjectDeleter{}(static_cast<VulkanPipeline*>(pipeline));
		m_pipelines.clear();
		for (dyf::RHI::ShaderHandle shader : m_shaders)
			VulkanObjectDeleter{}(static_cast<VulkanShader*>(shader));
		m_shaders.clear();
		for (dyf::RHI::TextureHandle texture : m_textures)
			VulkanObjectDeleter{}(static_cast<VulkanTexture*>(texture));
		m_textures.clear();
		for (dyf::RHI::BufferHandle buffer : m_buffers)
			VulkanObjectDeleter{}(static_cast<VulkanBuffer*>(buffer));
		m_buffers.clear();

		if (m_context.device != VK_NULL_HANDLE)
		{
			DestroyRetiredSwapchains();
			DestroyCurrentSwapchain();
			vkDestroyDevice(m_context.device, nullptr);
			m_context.device = VK_NULL_HANDLE;
		}
		if (m_context.surface != VK_NULL_HANDLE)
		{
			vkDestroySurfaceKHR(m_context.instance, m_context.surface, nullptr);
			m_context.surface = VK_NULL_HANDLE;
		}
		if (m_context.instance != VK_NULL_HANDLE)
		{
            if (m_debugMessenger != VK_NULL_HANDLE)
            {
                const auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(m_context.instance, "vkDestroyDebugUtilsMessengerEXT"));
                if (destroyMessenger) destroyMessenger(m_context.instance, m_debugMessenger, nullptr);
                m_debugMessenger = VK_NULL_HANDLE;
            }
			vkDestroyInstance(m_context.instance, nullptr);
			m_context.instance = VK_NULL_HANDLE;
		}
	}
}
