#pragma once
namespace dyf::RHI { struct DebugLabelColor; }
namespace dyf::Diagnostics
{
void BeginPixEvent(void* commandList,const char*,const RHI::DebugLabelColor&) noexcept;
void EndPixEvent(void* commandList) noexcept;
void InsertPixMarker(void* commandList,const char*,const RHI::DebugLabelColor&) noexcept;
}
