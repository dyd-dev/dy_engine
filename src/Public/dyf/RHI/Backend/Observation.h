#pragma once

#include "dyf/RHI/Binding.h"
#include <cstdint>
#include <memory>

namespace dyf::RHI::Backend
{
// Adapters report native work; collector IDs, events and GPU coverage stay in RHI.
class ReadbackTrace final
{
public:
#if defined(DY_LIVE_PROFILING_ENABLED) || defined(DY_PROFILER_TRACING_ENABLED)
    ReadbackTrace(TextureHandle texture,uint64_t bytes) noexcept;
    ~ReadbackTrace();
    void Copy() noexcept;
    void Submit() noexcept;
    void Wait() noexcept;
    void Finish(bool success=true) noexcept;
private:
    struct State;
    std::unique_ptr<State> m_state;
#else
    ReadbackTrace(TextureHandle,uint64_t) noexcept {}
    void Copy() noexcept {}
    void Submit() noexcept {}
    void Wait() noexcept {}
    void Finish(bool=true) noexcept {}
#endif
};

#ifdef DY_LIVE_PROFILING_ENABLED
void ObserveUploadPage(uint64_t bytes,bool reused) noexcept;
#else
inline void ObserveUploadPage(uint64_t,bool) noexcept {}
#endif
}
