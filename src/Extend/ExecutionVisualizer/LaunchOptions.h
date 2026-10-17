#pragma once
#include <atomic>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace viewer
{
struct ExecutableFile { std::string path,relativePath; };
struct ExecutableScan
{
    std::string root,error;
    std::vector<ExecutableFile> files;
    std::vector<std::string> issues;
    uint64_t errors=0,skippedLinks=0;
    bool cancelled=false;
};
ExecutableScan ScanExecutables(const std::string& root,const std::atomic_bool* cancel=nullptr);

enum class ArgumentType { Flag,Text,Integer,Float,Choice };
struct LaunchArgument
{
    std::string name,value,description;
    ArgumentType type=ArgumentType::Text;
    bool enabled=true;
    std::vector<std::string> choices;
    int64_t minimum=std::numeric_limits<int64_t>::min(),maximum=std::numeric_limits<int64_t>::max();
    double minimumFloat=std::numeric_limits<double>::lowest(),maximumFloat=std::numeric_limits<double>::max();
    bool exclusiveMinimum=false;
};
bool BuildArgumentString(const std::vector<LaunchArgument>&,std::string& arguments,std::string& error);
struct LaunchOptions
{
    std::string executable,directory,rawArguments,source,description;
    std::vector<LaunchArgument> arguments;
    bool useRaw=false,known=false;
    void SelectExecutable(const std::string& path);
    bool BuildArguments(std::string& result,std::string& error) const;
};
}
