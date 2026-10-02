#pragma once
#include <chrono>

namespace dyf::Platform
{
	class Time
	{
	public:
		// Steady-clock seconds. Paused ticks and the first resumed tick return zero.
		double Tick(bool paused = false)
		{
			const auto now = Clock::now();
			m_delta = paused || m_paused ? 0.0 : std::chrono::duration<double>(now - m_previous).count();
			m_previous = now;
			m_paused = paused;
			m_elapsed += m_delta;
			return m_delta;
		}
		void Reset()
		{
			m_previous = Clock::now();
			m_delta = m_elapsed = 0;
			m_paused = false;
		}
		[[nodiscard]] double GetDeltaSeconds() const { return m_delta; }
		[[nodiscard]] double GetElapsedSeconds() const { return m_elapsed; }

	private:
		using Clock = std::chrono::steady_clock;
		Clock::time_point m_previous = Clock::now();
		double m_delta = 0, m_elapsed = 0;
		bool m_paused = false;
	};
}
