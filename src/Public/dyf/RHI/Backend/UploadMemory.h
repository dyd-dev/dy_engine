#pragma once
#include "dyf/RHI/IDevice.h"

// Shared upload policy; native adapters only supply mapped page allocation.
namespace dyf::RHI::Detail { struct UploadPolicy { bool reusePages=true,suballocate=true; }; }
