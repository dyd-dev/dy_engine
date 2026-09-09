#pragma once

// Tracy's scoped zones are RAII objects: entering a scope starts a CPU zone and
// destroying the object at scope exit closes it, including on early returns.
#if defined(DY_TRACY_ENABLED)
#include <tracy/Tracy.hpp>
#define DY_PROFILE_CPU_ZONE() ZoneScoped
#define DY_PROFILE_CPU_ZONE_NAMED(name) ZoneScopedN(name)
#define DY_PROFILE_GPU_MILLISECONDS(name, value) TracyPlot(name, value)
#define DY_PROFILE_RESOURCE_COUNT(name, value) TracyPlot(name, static_cast<double>(value))
#else
#define DY_PROFILE_CPU_ZONE() ((void)0)
#define DY_PROFILE_CPU_ZONE_NAMED(name) ((void)sizeof(name))
#define DY_PROFILE_GPU_MILLISECONDS(name, value) do { (void)sizeof(name); (void)(value); } while(false)
#define DY_PROFILE_RESOURCE_COUNT(name, value) do { (void)sizeof(name); (void)(value); } while(false)
#endif
