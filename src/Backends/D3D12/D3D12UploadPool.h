#pragma once
#include "../../RHI/UploadPool.h"

namespace dyf::Backends::D3D12Detail
{
// All backends share the same page ownership, retirement and budget protocol.
using UploadPolicy=RHI::Detail::UploadPolicy;
template<class Page> using UploadPagePool=RHI::Detail::UploadPagePool<Page>;
template<class Page> using UploadArena=RHI::Detail::UploadArena<Page>;
}
