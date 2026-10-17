#include "ToolBootstrap.h"
#include "Process.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace viewer::tool_bootstrap
{
namespace fs = std::filesystem;

struct Package
{
    std::string name, directory;
    std::wstring url;
    std::string archiveSha, manifestSha;
    std::vector<std::string> required;
    std::wstring licenseUrl;
    std::string licenseSha;
};

// Tracy's archive digest is published by GitHub; RenderDoc's digest was measured from its official HTTPS portable release.
const Package TracyPackage{"Tracy 0.13.1", "tracy-0.13.1-win64",
    L"https://github.com/wolfpld/tracy/releases/download/v0.13.1/windows-0.13.1.zip",
    "ee6db1a7e71a12deb5973a8dbfdf9f36d3635bec0e0b31b1cc74f28de7dac4c9",
    "66bbec7f1617e876cb4fc5ab453b5b4d0defc6ee5c5b29de03e0e421238a1d62",
    {"tracy-profiler.exe", "LICENSE"},
    L"https://raw.githubusercontent.com/wolfpld/tracy/v0.13.1/LICENSE",
    "16f7df4828bd89c880c17dd2f45e834b754b6d5e8c9a9ee886af1be4b89475ca"};
const Package RenderDocPackage{"RenderDoc 1.37", "renderdoc-1.37-win64",
    L"https://renderdoc.org/stable/1.37/RenderDoc_1.37_64.zip",
    "6e72d62c3ddd91e3e607d205fb7761e47db34bed0c517b0262783cf5fa345953",
    "c9db72304fd8afcdab3ec2d4a94478d538449257ac9dff627d6eb128edeb470f",
    {"RenderDoc_1.37_64/renderdoccmd.exe", "RenderDoc_1.37_64/qrenderdoc.exe",
        "RenderDoc_1.37_64/renderdoc.dll", "RenderDoc_1.37_64/LICENSE.md"}, {}, {}};

void CheckCancelled(const std::atomic_bool& cancel)
{
    if(cancel.load(std::memory_order_relaxed)) throw std::runtime_error("Tool setup cancelled.");
}

void Progress(const ToolBootstrapProgress& progress, std::string message, const std::atomic_bool& cancel)
{
    CheckCancelled(cancel);
    if(progress) progress(message);
    CheckCancelled(cancel);
}

struct Handle
{
    HANDLE value = nullptr;
    ~Handle() { if(value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

std::runtime_error WindowsError(std::string message)
{
    return std::runtime_error(message + " (Win32 " + std::to_string(GetLastError()) + ").");
}

void CheckPath(const fs::path& path)
{
    auto current = path.root_path();
    for(const auto& part : path.relative_path())
    {
        current /= part;
        const auto attributes = GetFileAttributesW(current.c_str());
        if(attributes == INVALID_FILE_ATTRIBUTES)
        {
            const auto error = GetLastError();
            if(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return;
            throw WindowsError("Cannot inspect profiler cache path");
        }
        if(attributes & FILE_ATTRIBUTE_REPARSE_POINT)
            throw std::runtime_error("Profiler cache must not contain symbolic links or junctions: " + ToUtf8(current.native()));
    }
}

struct Stage
{
    fs::path path;
    bool owned = true;
    Stage(const fs::path& parent, std::string_view name)
    {
        CheckPath(parent);
        static std::atomic_uint64_t sequence{0};
        for(unsigned attempt = 0; attempt != 20; ++attempt)
        {
            path = parent / ToWide("." + std::string(name) + ".partial-" + std::to_string(GetCurrentProcessId())
                + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(sequence++));
            if(fs::create_directory(path)) return;
        }
        throw std::runtime_error("Cannot create a private profiler staging directory.");
    }
    ~Stage()
    {
        if(!owned) return;
        try { CheckPath(path); std::error_code error; fs::remove_all(path, error); }
        catch(const std::exception&) {}
    }
};

class Sha256
{
public:
    Sha256()
    {
        if(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
            throw std::runtime_error("Cannot open the Windows SHA256 provider.");
        if(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0)
        {
            BCryptCloseAlgorithmProvider(algorithm, 0);
            throw std::runtime_error("Cannot initialize SHA256.");
        }
    }
    ~Sha256() { BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0); }
    void Add(const void* bytes, ULONG count)
    {
        if(BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<void*>(bytes)), count, 0) < 0)
            throw std::runtime_error("Cannot compute SHA256.");
    }
    std::string Finish()
    {
        std::array<unsigned char, 32> digest{};
        if(BCryptFinishHash(hash, digest.data(), ULONG(digest.size()), 0) < 0)
            throw std::runtime_error("Cannot finish SHA256.");
        constexpr char digits[] = "0123456789abcdef";
        std::string result;
        for(const auto byte : digest) { result += digits[byte >> 4]; result += digits[byte & 15]; }
        return result;
    }
private:
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
};

std::string HashFile(const fs::path& path, const std::atomic_bool& cancel)
{
    CheckCancelled(cancel);
    CheckPath(path);
    std::ifstream file(path, std::ios::binary);
    if(!file) throw std::runtime_error("Cannot read profiler cache file: " + ToUtf8(path.native()));
    Sha256 hash;
    std::array<char, 65536> bytes{};
    while(file)
    {
        CheckCancelled(cancel);
        file.read(bytes.data(), bytes.size());
        if(const auto count = file.gcount()) hash.Add(bytes.data(), ULONG(count));
    }
    if(!file.eof()) throw std::runtime_error("Failed to read profiler cache file.");
    return hash.Finish();
}

std::string HashText(std::string_view text)
{
    Sha256 hash;
    if(text.size() > 1024 * 1024) throw std::runtime_error("Profiler file manifest is too large.");
    hash.Add(text.data(), ULONG(text.size()));
    return hash.Finish();
}

bool HashMatches(const fs::path& path, std::string_view expected, const std::atomic_bool& cancel)
{
    CheckPath(path);
    try { return fs::is_regular_file(path) && HashFile(path, cancel) == expected; }
    catch(const std::exception&) { CheckCancelled(cancel); return false; }
}

std::string Lower(std::string text)
{
    for(auto& c : text) if(c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return text;
}

bool SafeArchivePath(std::string_view path)
{
    if(path.empty() || path.front() == '/') return false;
    if(path.back() == '/') path.remove_suffix(1);
    if(path.empty()) return false;
    while(!path.empty())
    {
        const auto slash = path.find('/');
        const auto part = path.substr(0, slash);
        if(part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ') return false;
        for(const unsigned char c : part)
            if(c < 32 || c == 127 || std::string_view("<>:\"\\|?*").find(char(c)) != std::string_view::npos) return false;
        const auto device = Lower(std::string(part.substr(0, part.find('.'))));
        if(device == "con" || device == "prn" || device == "aux" || device == "nul" || device == "clock$"
            || (device.size() == 4 && (device.substr(0, 3) == "com" || device.substr(0, 3) == "lpt") && device[3] >= '1' && device[3] <= '9')) return false;
        if(slash == std::string_view::npos) return true;
        path.remove_prefix(slash + 1);
        if(path.empty()) return false;
    }
    return false;
}

std::string RunSystemTool(std::wstring_view name, const std::vector<std::wstring>& arguments, const std::atomic_bool& cancel)
{
    CheckCancelled(cancel);
    wchar_t systemDirectory[MAX_PATH]{};
    const auto length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if(!length || length >= MAX_PATH) throw WindowsError("Cannot find the Windows system directory");
    const auto executable = fs::path(systemDirectory) / name;
    std::wstring command = QuoteArgument(executable.native());
    for(const auto& argument : arguments) command += L" " + QuoteArgument(argument);
    if(command.size() >= 32767) throw std::runtime_error("Profiler helper command is too long.");
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle output, writer, input;
    if(!CreatePipe(&output.value, &writer.value, &security, 0)
        || !SetHandleInformation(output.value, HANDLE_FLAG_INHERIT, 0)) throw WindowsError("Cannot create the profiler helper output pipe");
    input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
    if(input.value == INVALID_HANDLE_VALUE) throw WindowsError("Cannot open the profiler helper input");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input.value;
    startup.hStdOutput = startup.hStdError = writer.value;
    PROCESS_INFORMATION created{};
    if(!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        nullptr, nullptr, &startup, &created)) throw WindowsError("Cannot start Windows " + ToUtf8(name));
    struct Child
    {
        HANDLE process;
        ~Child()
        {
            if(WaitForSingleObject(process, 0) == WAIT_TIMEOUT)
            { TerminateProcess(process, ERROR_CANCELLED); WaitForSingleObject(process, 2000); }
            CloseHandle(process);
        }
    } child{created.hProcess};
    CloseHandle(created.hThread);
    CloseHandle(writer.value); writer.value = nullptr;
    const auto begin = GetTickCount64();
    std::string result;
    const auto drain = [&]
    {
        for(;;)
        {
            CheckCancelled(cancel);
            DWORD available = 0;
            if(!PeekNamedPipe(output.value, nullptr, 0, nullptr, &available, nullptr))
            {
                if(GetLastError() == ERROR_BROKEN_PIPE) return;
                throw WindowsError("Cannot inspect profiler helper output");
            }
            if(!available) return;
            std::array<char, 4096> bytes{};
            DWORD count = 0;
            if(!ReadFile(output.value, bytes.data(), std::min<DWORD>(available, DWORD(bytes.size())), &count, nullptr))
                throw WindowsError("Cannot read profiler helper output");
            result.append(bytes.data(), count);
            if(result.size() > 1024 * 1024) throw std::runtime_error("Profiler helper output exceeded its limit.");
        }
    };
    for(;;)
    {
        drain();
        const auto state = WaitForSingleObject(child.process, 100);
        if(state == WAIT_OBJECT_0) { drain(); break; }
        if(state != WAIT_TIMEOUT) throw WindowsError("Cannot wait for the profiler helper");
        if(GetTickCount64() - begin > 180000) throw std::runtime_error("Profiler helper timed out. Retry setup.");
    }
    DWORD exit = 0;
    if(!GetExitCodeProcess(child.process, &exit)) throw WindowsError("Cannot read profiler helper exit status");
    if(exit) throw std::runtime_error("Windows " + ToUtf8(name) + " failed (" + std::to_string(exit) + "): " + result);
    return result;
}

using Download = std::function<void(const std::wstring&, const fs::path&, const std::atomic_bool&)>;

void DownloadFile(const std::wstring& url, const fs::path& destination, const std::atomic_bool& cancel)
{
    RunSystemTool(L"curl.exe", {L"--disable", L"--fail", L"--location", L"--silent", L"--show-error", L"--proto", L"=https",
        L"--proto-redir", L"=https", L"--tlsv1.2", L"--connect-timeout", L"15", L"--max-time", L"180",
        L"--max-filesize", L"268435456", L"--output", destination.native(), url}, cancel);
}

std::vector<std::string> Lines(std::string_view text)
{
    std::vector<std::string> lines;
    std::istringstream stream{std::string(text)};
    std::string line;
    while(std::getline(stream, line))
    {
        if(!line.empty() && line.back() == '\r') line.pop_back();
        if(line.empty()) throw std::runtime_error("Invalid empty archive or manifest entry.");
        lines.push_back(std::move(line));
    }
    return lines;
}

void ValidateArchive(const fs::path& archive, const std::atomic_bool& cancel)
{
    const auto names = Lines(RunSystemTool(L"tar.exe", {L"-tf", archive.native()}, cancel));
    const auto types = Lines(RunSystemTool(L"tar.exe", {L"-tvf", archive.native()}, cancel));
    if(names.empty() || names.size() != types.size()) throw std::runtime_error("Profiler archive has an invalid file list.");
    std::set<std::string> unique;
    for(size_t i = 0; i != names.size(); ++i)
    {
        auto key = Lower(names[i]);
        if(!key.empty() && key.back() == '/') key.pop_back();
        if(!SafeArchivePath(names[i]) || key == ".archive.zip" || key == ".files.sha256" || !unique.insert(key).second
            || (types[i].front() != '-' && types[i].front() != 'd'))
            throw std::runtime_error("Profiler archive contains an unsafe path, link or duplicate entry: " + names[i]);
    }
}

std::string BuildManifest(const fs::path& directory, const std::atomic_bool& cancel)
{
    std::vector<std::string> names;
    for(const auto& entry : fs::recursive_directory_iterator(directory))
    {
        CheckCancelled(cancel);
        CheckPath(entry.path());
        if(entry.is_directory()) continue;
        if(!entry.is_regular_file()) throw std::runtime_error("Profiler archive contains a non-file entry.");
        const auto relative = ToUtf8(entry.path().lexically_relative(directory).generic_wstring());
        if(!SafeArchivePath(relative)) throw std::runtime_error("Profiler archive contains an unsafe extracted path.");
        if(relative != ".archive.zip" && relative != ".files.sha256") names.push_back(relative);
    }
    std::sort(names.begin(), names.end());
    std::string manifest;
    for(const auto& name : names) manifest += HashFile(directory / fs::u8path(name), cancel) + " " + name + "\n";
    return manifest;
}

bool ValidCache(const fs::path& directory, const Package& package, const std::atomic_bool& cancel)
{
    CheckCancelled(cancel);
    CheckPath(directory);
    if(!HashMatches(directory / L".files.sha256", package.manifestSha, cancel)) return false;
    if(HashText(BuildManifest(directory, cancel)) != package.manifestSha) return false;
    for(const auto& name : package.required)
        if(!fs::is_regular_file(directory / fs::u8path(name))) return false;
    return true;
}

void Rename(const fs::path& from, const fs::path& to, const std::atomic_bool& cancel)
{
    const auto begin = GetTickCount64();
    for(;;)
    {
        CheckCancelled(cancel);
        if(MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) return;
        const auto error = GetLastError();
        // Windows can briefly deny a new executable directory rename while another process has it open.
        if((error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION) || GetTickCount64() - begin >= 2000)
        { SetLastError(error); throw WindowsError("Cannot atomically publish profiler tools"); }
        Sleep(50);
    }
}

void QuarantineRenderDocBytecode(const fs::path& cache, const fs::path& directory,
    const ToolBootstrapProgress& progress, const std::atomic_bool& cancel)
{
    CheckCancelled(cancel);
    const auto bytecode = directory / L"RenderDoc_1.37_64/PySide2/__pycache__";
    std::vector<fs::path> generated;
    for(const auto* name : {L"__init__.cpython-36.pyc", L"_utils.cpython-36.pyc"})
    {
        const auto file = bytecode / name;
        CheckPath(file);
        if(!fs::exists(file)) continue;
        if(!fs::is_regular_file(file)) throw std::runtime_error("Unexpected RenderDoc Python bytecode cache entry.");
        generated.push_back(file);
    }
    if(generated.empty()) return;
    Progress(progress, "Preserving RenderDoc generated Python bytecode outside its executable directory...", cancel);
    Stage preserved(cache, "renderdoc-bytecode");
    auto name = preserved.path.filename().native();
    name.replace(name.find(L".partial-"), 9, L".quarantine-");
    const auto quarantine = cache / name;
    Rename(preserved.path, quarantine, cancel);
    preserved.owned = false;
    // RenderDoc 1.37 ignores Python environment flags; never trust its previously generated import cache.
    for(const auto& file : generated) Rename(file, quarantine / file.filename(), cancel);
}

fs::path PrepareTool(const fs::path& cache, const Package& package, const ToolBootstrapProgress& progress,
    const std::atomic_bool& cancel, const Download& download)
{
    try
    {
        const auto destination = cache / ToWide(package.directory);
        Progress(progress, "Verifying " + package.name + " cache...", cancel);
        if(package.directory == RenderDocPackage.directory && HashMatches(destination / L".files.sha256", package.manifestSha, cancel))
            QuarantineRenderDocBytecode(cache, destination, progress, cancel);
        if(ValidCache(destination, package, cancel)) return destination;
        Stage stage(cache, package.directory);
        const auto archive = stage.path / L".archive.zip";
        if(HashMatches(destination / L".archive.zip", package.archiveSha, cancel))
        {
            Progress(progress, "Restoring " + package.name + " from its verified cached archive...", cancel);
            fs::copy_file(destination / L".archive.zip", archive);
        }
        else
        {
            Progress(progress, "Downloading official " + package.name + " portable archive...", cancel);
            download(package.url, archive, cancel);
        }
        if(!HashMatches(archive, package.archiveSha, cancel)) throw std::runtime_error("Official archive SHA256 mismatch; no files were installed.");
        Progress(progress, "Checking " + package.name + " archive paths...", cancel);
        ValidateArchive(archive, cancel);
        Progress(progress, "Extracting " + package.name + " portable files...", cancel);
        RunSystemTool(L"tar.exe", {L"-xf", archive.native(), L"-C", stage.path.native(), L"--no-same-owner", L"--no-same-permissions"}, cancel);
        if(!package.licenseUrl.empty())
        {
            const auto license = stage.path / L"LICENSE";
            if(HashMatches(destination / L"LICENSE", package.licenseSha, cancel)) fs::copy_file(destination / L"LICENSE", license);
            else
            {
                Progress(progress, "Downloading official " + package.name + " license...", cancel);
                download(package.licenseUrl, license, cancel);
            }
            if(!HashMatches(license, package.licenseSha, cancel)) throw std::runtime_error("Official license SHA256 mismatch.");
        }
        Progress(progress, "Verifying all " + package.name + " files and licenses...", cancel);
        const auto manifest = BuildManifest(stage.path, cancel);
        if(HashText(manifest) != package.manifestSha) throw std::runtime_error("Extracted file manifest does not match the pinned official release.");
        std::ofstream file(stage.path / L".files.sha256", std::ios::binary);
        file.write(manifest.data(), std::streamsize(manifest.size()));
        file.close();
        if(!file) throw std::runtime_error("Cannot save profiler file manifest.");
        for(const auto& name : package.required)
            if(!fs::is_regular_file(stage.path / fs::u8path(name))) throw std::runtime_error("Official release is missing a required executable or license.");
        CheckCancelled(cancel);
        CheckPath(destination);
        fs::path previous;
        if(fs::exists(destination))
        {
            // Preserve unknown user files when repairing a cache; only our private staging directory is removed.
            auto name = stage.path.filename().native();
            name.replace(name.find(L".partial-"), 9, L".previous-");
            previous = cache / name;
            Rename(destination, previous, cancel);
        }
        try { Rename(stage.path, destination, cancel); }
        catch(...)
        {
            if(!previous.empty() && !fs::exists(destination))
            {
                const std::atomic_bool rollbackCancel{false};
                try { Rename(previous, destination, rollbackCancel); }
                catch(const std::exception&) {}
            }
            throw;
        }
        stage.owned = false;
        Progress(progress, package.name + " portable tools ready.", cancel);
        return destination;
    }
    catch(const std::exception& error)
    { throw std::runtime_error(package.name + " setup failed: " + error.what() + " Retry tool setup."); }
}
}

namespace viewer
{
ToolPaths EnsureProfilerTools(const std::filesystem::path& cacheRoot,
    const ToolBootstrapProgress& progress, const std::atomic_bool& cancel)
{
    using namespace tool_bootstrap;
    CheckCancelled(cancel);
#if !defined(_M_X64) && !defined(__x86_64__)
    throw std::runtime_error("Automatic profiler tool setup requires Windows x64.");
#endif
    if(cacheRoot.empty() || cacheRoot.native().find(L'\0') != std::wstring::npos)
        throw std::runtime_error("Select a valid local profiler tools cache directory.");
    const auto cache = fs::absolute(cacheRoot).lexically_normal();
    CheckPath(cache);
    fs::create_directories(cache);
    const auto lockFile = cache / L".dy-profiler-tools.lock";
    CheckPath(lockFile);
    Handle lock;
    lock.value = CreateFileW(lockFile.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if(lock.value == INVALID_HANDLE_VALUE)
        throw WindowsError("Another analyzer may be preparing profiler tools; retry setup shortly");
    const auto renderdoc = PrepareTool(cache, RenderDocPackage, progress, cancel, DownloadFile) / L"RenderDoc_1.37_64";
    return {{}, renderdoc / L"renderdoccmd.exe", renderdoc / L"qrenderdoc.exe"};
}
}
