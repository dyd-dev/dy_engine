#include "dyf/RHI/IDevice.h"

using namespace dyf::RHI;

bool IDevice::Supports(Feature feature) const {return SupportsNative(feature);}
