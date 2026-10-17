#include "../Process.h"
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

static void Check(bool value, const char* message)
{
    if(!value) { std::fprintf(stderr, "FAIL %s\n", message); throw std::runtime_error(message); }
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        if(argc > 1 && std::wstring(argv[1]) == L"--child")
        {
            return argc == 5 && std::filesystem::current_path() == std::filesystem::path(argv[2])
                && std::wstring(argv[3]) == L"한글 공백\\\"text\\" && std::wstring(argv[4]).empty() ? 47 : 48;
        }
        for(const auto& value : {L"", L"plain", L"a b", L"a\"b", L"ends \\", L"한글 경로\\\\\"x"})
        {
            const auto command = L"test.exe " + viewer::QuoteArgument(value);
            int count = 0;
            auto** parsed = CommandLineToArgvW(command.c_str(), &count);
            const bool passed = parsed && count == 2 && std::wstring(parsed[1]) == value;
            if(parsed) LocalFree(parsed);
            Check(passed, "Windows argument round trip, including empty, quotes and backslashes");
        }
        Check(viewer::ToUtf8(viewer::ToWide("한글 경로")) == "한글 경로", "UTF-8 round trip");
        for(const auto& invalid : {std::string("\xC0\xAF"), std::string("a\0b", 3)})
        {
            bool rejected = false;
            try { (void)viewer::ToWide(invalid); } catch(const std::invalid_argument&) { rejected = true; }
            Check(rejected, "Invalid UTF-8 and embedded NUL rejected");
        }
        wchar_t ownPath[32768]{};
        Check(GetModuleFileNameW(nullptr, ownPath, 32768) > 0, "Locate process test executable");
        const auto folder = std::filesystem::absolute(std::filesystem::path(ownPath).parent_path() / L"한글 실행 경로");
        std::filesystem::create_directories(folder);
        const auto target = folder / L"테스트 도구.exe";
        std::filesystem::copy_file(ownPath, target, std::filesystem::copy_options::overwrite_existing);
        const auto arguments = L"--child " + viewer::QuoteArgument(folder.native()) + L" "
            + viewer::QuoteArgument(L"한글 공백\\\"text\\") + L" " + viewer::QuoteArgument(L"");
        viewer::TargetProcess process;
        std::string error;
        Check(process.Launch(viewer::ToUtf8(target.native()), viewer::ToUtf8(arguments), viewer::ToUtf8(folder.native()), error), error.c_str());
        const auto deadline = GetTickCount64() + 5000;
        while(process.Running() && GetTickCount64() < deadline) Sleep(5);
        Check(process.ExitCode() == 47u, "Real CreateProcessW UTF-8 path, cwd and arguments");
        Check(!process.Launch("missing-target.exe", "", "", error) && !error.empty(), "Launch failure reported");
        Check(process.ExitCode() == 47u, "Failed launch preserves previous process result");
        std::puts("PASS quoting, invalid UTF-8, real Korean executable/cwd/arguments, process exit and failure preservation");
    }
    catch(const std::exception&) { return 1; }
    return 0;
}
