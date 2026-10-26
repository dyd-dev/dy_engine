#include "dyf/RHI/IDevice.h"

using namespace dyf::RHI;

bool IDevice::Supports(Feature feature) const {return SupportsNative(feature);}

bool IDevice::Supports(Format format,FormatFeature feature) const noexcept
{
    return SupportsFormatNative(format,feature);
}
