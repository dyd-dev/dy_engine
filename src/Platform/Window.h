#pragma once
struct GLFWwindow;

namespace dy::Platform
{
	enum class Key
	{
		F11
	};

	class Window
	{
	public:
		~Window();

		Window(unsigned int width, unsigned int height);
		Window(unsigned int width, unsigned int height, const char *title);

		bool IsRunning() const;
		void PollEvents() const;
		void Resize(unsigned int width, unsigned int height) const;
		// Returns true once for each physical key press. Renderer uses this shared
		// event so the built-in profiler HUD works without per-application wiring.
		[[nodiscard]] static bool ConsumeKeyPress(Key key);

		void* GetHandle() const;

	private:
		struct GLFWwindow* m_window;
	};
}
