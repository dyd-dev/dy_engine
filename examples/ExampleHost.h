#pragma once
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>
#include <memory>

namespace dyf::Platform { class Window; }

namespace dy_examples
{
struct Example
{
    const char* name;
    const char* executable;
    const char* category;
    int (*entry)(int, char**) = nullptr;
};

// The caller retains one native window. Visits release GPU resources while
// preserving the workbench's scene and layout in CPU memory.
struct WorkbenchState;
int OpenWorkbench(dyf::Platform::Window&, std::shared_ptr<WorkbenchState>&,
    const Example*, std::size_t, const Example*& selected,
    std::vector<std::string>& arguments, const std::string& status,
    const std::filesystem::path& assetRoot, const std::filesystem::path& repositoryRoot,
    const std::filesystem::path& initialScene = {});

// Startup controls edit argv locally. Applying them restarts the module in the
// same native window, preserving the root scene and window layout.
void SetExampleArguments(const Example&, const std::vector<std::string>&);
void EditExampleArguments(const Example&);
std::vector<std::string> GetExampleArguments(const Example&);

struct ExampleResult
{
    int code=0;
    const Example* next=nullptr;
    std::vector<std::string> arguments;
    std::string message;
};
ExampleResult RunHostedExample(dyf::Platform::Window&,const Example*,std::size_t,
    const Example&,const std::vector<std::string>&,const std::filesystem::path& repositoryRoot);
}
