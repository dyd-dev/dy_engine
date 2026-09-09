// 측정 흐름 설계: 현재 main의 API와 대조한 뒤 별도로 활성화한다. CMake 대상이 아니다.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <charconv>
#include <limits>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "Math/Math.h"

using namespace dy;

namespace
{
	using Clock = std::chrono::steady_clock;
	using BatchFunction = void (*)(const Math::float4x4*, const Math::float4x4*, Math::float4x4*, size_t);

#if defined(_MSC_VER)
	#define DY_BENCH_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
	#define DY_BENCH_NOINLINE __attribute__((noinline))
#else
	#define DY_BENCH_NOINLINE
#endif

	struct Options
	{
		uint32_t matrixCount = 100000u;
		uint32_t warmUpCount = 5u;
		uint32_t repeatCount = 20u;
	};

	struct Measurement
	{
		double totalMilliseconds = 0.0;
		double nanosecondsPerMatrix = 0.0;
		float checksum = 0.0f;
	};

	volatile float g_sink = 0.0f;

	uint32_t ParsePositiveArgument(int argc, char** argv, const char* name, uint32_t fallback)
	{
		const std::string prefix = std::string(name) + "=";
		for(int i = 1; i < argc; ++i)
		{
			const std::string argument = argv[i];
			if(argument.rfind(prefix, 0) != 0) continue;
			uint32_t value = 0;
			const auto result = std::from_chars(argument.data() + prefix.size(), argument.data() + argument.size(), value);
			if(result.ec != std::errc{} || result.ptr != argument.data() + argument.size() || value == 0)
				throw std::invalid_argument("Expected a positive uint32 for " + std::string(name));
			return value;
		}
		return fallback;
	}

	Options ParseOptions(int argc, char** argv)
	{
		Options options;
		options.matrixCount = static_cast<uint32_t>(ParsePositiveArgument(argc, argv, "--count", options.matrixCount));
		options.warmUpCount = static_cast<uint32_t>(ParsePositiveArgument(argc, argv, "--warmup", options.warmUpCount));
		options.repeatCount = static_cast<uint32_t>(ParsePositiveArgument(argc, argv, "--repeat", options.repeatCount));
		if(options.repeatCount > std::numeric_limits<uint32_t>::max() - options.warmUpCount)
			throw std::invalid_argument("warmup + repeat overflows uint32");
		return options;
	}

	const char* BuildConfiguration()
	{
#if defined(NDEBUG)
		return "Release (NDEBUG)";
#else
		return "Debug (assertions enabled)";
#endif
	}

	const char* SimdConfiguration()
	{
#if defined(DY_SIMD_X64)
		return "x64 SIMD";
#elif defined(DY_SIMD_ARM64)
		return "ARM64 NEON";
#elif defined(DY_SIMD_ENABLED)
		return "enabled, scalar fallback on this architecture";
#else
		return "disabled (scalar fallback)";
#endif
	}

	// Compiler auto-vectorization을 막지 않는다. 단순 loop reference이며
	// scalar 구현을 보장하지 않는다.
	DY_BENCH_NOINLINE void MultiplyReferenceBatch(
		const Math::float4x4* lhs,
		const Math::float4x4* rhs,
		Math::float4x4* output,
		size_t count)
	{
		for(size_t index = 0; index < count; ++index)
		{
			for(size_t row = 0; row < 4u; ++row)
			{
				for(size_t column = 0; column < 4u; ++column)
				{
					float value = 0.0f;
					for(size_t inner = 0; inner < 4u; ++inner)
						value += lhs[index].m[row * 4u + inner] * rhs[index].m[inner * 4u + column];
					output[index].m[row * 4u + column] = value;
				}
			}
		}
	}

	DY_BENCH_NOINLINE void MultiplyPublicBatch(
		const Math::float4x4* lhs,
		const Math::float4x4* rhs,
		Math::float4x4* output,
		size_t count)
	{
		Math::MultiplyMatricesBatch(lhs, rhs, output, count);
	}

	void ValidateResults(const std::vector<Math::float4x4>& lhs, const std::vector<Math::float4x4>& rhs)
	{
		std::vector<Math::float4x4> expected(lhs.size()), actual(lhs.size());
		MultiplyReferenceBatch(lhs.data(), rhs.data(), expected.data(), lhs.size());
		MultiplyPublicBatch(lhs.data(), rhs.data(), actual.data(), lhs.size());
		for(size_t matrix = 0; matrix < lhs.size(); ++matrix)
			for(size_t element = 0; element < 16; ++element)
			{
				const float reference = expected[matrix].m[element];
				const float result = actual[matrix].m[element];
				const float tolerance = 1.0e-5f * (1.0f + std::abs(reference));
				if(!std::isfinite(result) || std::abs(result - reference) > tolerance)
					throw std::runtime_error("Matrix batch result differs from loop reference");
			}
	}

	void Consume(float value)
	{
		g_sink = g_sink + value;
		std::atomic_signal_fence(std::memory_order_seq_cst);
	}

	Measurement Measure(
		BatchFunction function,
		const std::vector<Math::float4x4>& initialLhs,
		const std::vector<Math::float4x4>& rhs,
		uint32_t warmUpCount,
		uint32_t repeatCount)
	{
		std::vector<Math::float4x4> lhs = initialLhs;
		std::vector<Math::float4x4> output(lhs.size());

		auto runOnce = [&](uint32_t iteration)
		{
			const size_t changedIndex = static_cast<size_t>(iteration) % lhs.size();
			lhs[changedIndex].m[12] += 0.000001f;
			function(lhs.data(), rhs.data(), output.data(), output.size());
			Consume(output[changedIndex].m[static_cast<size_t>(iteration) % 16u]);
		};

		for(uint32_t iteration = 0; iteration < warmUpCount; ++iteration) runOnce(iteration);

		const auto start = Clock::now();
		for(uint32_t iteration = 0; iteration < repeatCount; ++iteration)
			runOnce(iteration + warmUpCount);
		const auto end = Clock::now();

		float checksum = 0.0f;
		for(const Math::float4x4& matrix : output)
			checksum += matrix.m[0] + matrix.m[5] + matrix.m[10] + matrix.m[12];
		Consume(checksum);

		const double totalMilliseconds = std::chrono::duration<double, std::milli>(end - start).count();
		const double operationCount = static_cast<double>(lhs.size()) * static_cast<double>(repeatCount);
		return Measurement{
			totalMilliseconds,
			totalMilliseconds * 1000000.0 / operationCount,
			checksum
		};
	}

	void PrintMeasurement(const char* name, const Measurement& measurement)
	{
		std::cout << std::left << std::setw(24) << name
		          << " total=" << std::right << std::setw(10) << measurement.totalMilliseconds << " ms"
		          << ", " << std::setw(10) << measurement.nanosecondsPerMatrix << " ns/matrix"
		          << ", checksum=" << measurement.checksum << '\n';
	}
}

int main(int argc, char** argv)
{
	try
	{
		const Options options = ParseOptions(argc, argv);
		std::vector<Math::float4x4> lhs(options.matrixCount);
		std::vector<Math::float4x4> rhs(options.matrixCount);

		for(size_t index = 0; index < options.matrixCount; ++index)
		{
			const float value = static_cast<float>(index) * 0.0001f;
			lhs[index] = Math::RotationX(value);
			lhs[index].m[12] = value;
			lhs[index].m[13] = value * 0.5f;
			rhs[index] = Math::RotationZ(value * 0.7f);
			rhs[index].m[14] = -value * 0.25f;
		}

		std::cout << "Math performance\n"
		          << "  build       : " << BuildConfiguration() << '\n'
		          << "  public path : " << SimdConfiguration() << '\n'
		          << "  input       : " << options.matrixCount << " matrix pairs\n"
		          << "  warm-up     : " << options.warmUpCount << " batches\n"
		          << "  repeats     : " << options.repeatCount << " batches\n"
		          << "  units       : ms total, ns per matrix multiply\n\n";

		// 정확성 확인과 임시 buffer 할당은 측정 구간 밖에서 수행한다.
		ValidateResults(lhs, rhs);
		const Measurement reference = Measure(
			MultiplyReferenceBatch, lhs, rhs, options.warmUpCount, options.repeatCount);
		const Measurement publicBatch = Measure(
			MultiplyPublicBatch, lhs, rhs, options.warmUpCount, options.repeatCount);

		std::cout << std::fixed << std::setprecision(3);
		PrintMeasurement("loop reference", reference);
		PrintMeasurement("Math public batch", publicBatch);
		std::cout << "ratio (reference/public) "
		          << reference.nanosecondsPerMatrix / publicBatch.nanosecondsPerMatrix << " x\n";

		return 0;
	}
	catch(const std::exception& exception)
	{
		std::cerr << exception.what() << '\n';
		return 1;
	}
}
