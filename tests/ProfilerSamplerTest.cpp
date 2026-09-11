#include <cmath>

#include "Graphics/ProfilerSampler.h"

namespace
{
	bool Near(double lhs, double rhs, double tolerance)
	{
		return std::abs(lhs - rhs) <= tolerance;
	}
}

#define CHECK(expression) do { if(!(expression)) return __LINE__; } while(false)

int main()
{
	using namespace dy::Graphics;

	ProfilerSampler sampler(200.0, 1000.0);
	ProfilerTimingSnapshot snapshot = {};
	for(int frame = 0; frame < 11; ++frame)
	{
		CHECK(!sampler.AddSample({ 16.666, 4.0, 2.0, 1.0, true, true }, snapshot));
	}
	CHECK(sampler.AddSample({ 16.666, 8.0, 4.0, 3.0, true, true }, snapshot));
	CHECK(snapshot.frameCount == 12u);
	CHECK(Near(snapshot.fps, 60.0, 0.01));
	CHECK(Near(snapshot.frameAverageMilliseconds, 16.666, 0.001));
	CHECK(Near(snapshot.frameMaximumMilliseconds, 16.666, 0.001));
	CHECK(snapshot.hasGpuMain && snapshot.hasGpuShadow);
	CHECK(snapshot.cpuRenderAverageMilliseconds > 4.0 && snapshot.cpuRenderAverageMilliseconds < 4.4);

	for(int frame = 0; frame < 10; ++frame)
	{
		CHECK(!sampler.AddSample({ 16.666, 4.0, 2.0, 1.0, true, true }, snapshot));
	}
	CHECK(sampler.AddSample({ 40.0, 9.0, 3.0, 2.0, true, true }, snapshot));
	CHECK(Near(snapshot.frameMaximumMilliseconds, 40.0, 0.001));
	CHECK(snapshot.frameAverageMilliseconds > 18.0 && snapshot.frameAverageMilliseconds < 19.0);

	CHECK(!sampler.AddSample({ 1200.0, 1200.0, 1200.0, 1200.0, true, true }, snapshot));
	for(int frame = 0; frame < 11; ++frame)
	{
		CHECK(!sampler.AddSample({ 16.666, 3.0, 0.0, 0.0, false, false }, snapshot));
	}
	CHECK(sampler.AddSample({ 16.666, 3.0, 0.0, 0.0, false, false }, snapshot));
	CHECK(snapshot.frameMaximumMilliseconds < 17.0);
	CHECK(!snapshot.hasGpuMain && !snapshot.hasGpuShadow);

	return 0;
}
