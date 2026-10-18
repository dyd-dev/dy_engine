#pragma once
#include "dyf/RHI/IDevice.h"

namespace dyf::RHI::Detail { struct UploadPolicy { bool reusePages=true,suballocate=true; }; }
