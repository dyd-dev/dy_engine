#pragma once
#include "dyf/RHI/Format.h"

namespace dyf::RHI
{
class IDevice;
namespace Detail
{
// Internal optional optimization; unknown backends retain manual filtering.
bool SupportsLinearFiltering(const IDevice&,Format) noexcept;
}
}
