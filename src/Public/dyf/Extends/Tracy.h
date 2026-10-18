#pragma once

namespace dyf::Platform { struct TraceSink; }

namespace dyf
{
	class TracyAdapter final
	{
	public:
		TracyAdapter() noexcept;
		~TracyAdapter();
		TracyAdapter(const TracyAdapter&) = delete;
		TracyAdapter& operator=(const TracyAdapter&) = delete;

	private:
		const Platform::TraceSink* sink = nullptr;
	};
}
