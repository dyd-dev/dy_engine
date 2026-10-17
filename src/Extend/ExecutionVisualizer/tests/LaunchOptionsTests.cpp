#include "../LaunchOptions.h"
#include "../Process.h"
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace fs=std::filesystem;
using viewer::ArgumentType;
using viewer::LaunchArgument;
static void Check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
static std::string Utf8(const fs::path& path) { return viewer::ToUtf8(path.native()); }
static LaunchArgument Arg(std::string name,std::string value,ArgumentType type)
{ LaunchArgument result; result.name=std::move(name); result.value=std::move(value); result.type=type; return result; }
int wmain(int argc,wchar_t** argv)
{
    try
    {
        if(argc>1 && std::wstring(argv[1])==L"--generated-child")
            return argc==12 && std::wstring(argv[2])==L"--cwd" && fs::current_path()==fs::path(argv[3])
                && std::wstring(argv[4])==L"--text" && std::wstring(argv[5])==L"한글 공백\\\"text\\"
                && std::wstring(argv[6])==L"--empty" && std::wstring(argv[7]).empty()
                && std::wstring(argv[8])==L"--integer" && std::wstring(argv[9])==L"-17"
                && std::wstring(argv[10])==L"--float" && std::wstring(argv[11])==L"1.25e-3" ? 61 : 62;
        wchar_t ownPath[32768]{}; Check(GetModuleFileNameW(nullptr,ownPath,32768)>0,"Locate test child");
        const auto root=fs::path(ownPath).parent_path()/L"Launcher 계약"/(std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        fs::create_directories(root/L"한글 공백"/L"nested"); fs::create_directories(root/L"second");
        std::ofstream(root/L"한글 공백"/L"same.exe").put('x');
        std::ofstream(root/L"second"/L"same.EXE").put('x');
        std::ofstream(root/L"한글 공백"/L"nested"/L"other.exe").put('x');
        std::ofstream(root/L"ignored.txt").put('x');
        const auto scan=viewer::ScanExecutables(Utf8(root));
        Check(scan.error.empty() && scan.files.size()==3,"Recursive catalog must include every nested .exe and case-insensitive duplicate name");
        Check(std::count_if(scan.files.begin(),scan.files.end(),[](const auto& entry){return entry.relativePath.find("same.")!=std::string::npos;})==2,
            "Same names remain distinguishable by relative path");
        Check(viewer::ScanExecutables("").error.size()>0,"Empty folder must not silently scan cwd");
        Check(viewer::ScanExecutables(Utf8(root/L"missing")).error.size()>0,"Missing folder reports an error");
        Check(viewer::ScanExecutables(Utf8(root/L"ignored.txt")).error.size()>0,"A file is not a scan folder");
        std::atomic_bool cancelled{true}; Check(viewer::ScanExecutables(Utf8(root),&cancelled).cancelled,"Scan cancellation is observed");
        std::error_code linkError; fs::create_directory_symlink(root,root/L"second"/L"cycle",linkError);
        if(!linkError)
        {
            const auto linked=viewer::ScanExecutables(Utf8(root));
            Check(linked.files.size()==3 && linked.skippedLinks==1 && !linked.issues.empty(),"Directory link cycles are skipped and reported");
        }
        else std::printf("SKIP directory symlink fixture: %s\n",linkError.message().c_str());
        fs::create_directory(root/L"empty");
        Check(viewer::ScanExecutables(Utf8(root/L"empty")).files.empty(),"Empty directories are valid empty catalogs");

        viewer::LaunchOptions options; options.SelectExecutable(Utf8(root/L"AdvancedInstancingBatching.exe"));
        Check(options.known && fs::path(viewer::ToWide(options.directory))==root,"Known executable sets parent cwd");
        std::string built,error;
        Check(options.BuildArguments(built,error) && built=="\"--mode\" \"instanced\"","Representative example14 keeps its existing default instanced mode and no extra flags");
        Check(!options.description.empty(),"Representative example14 explains its live instancing comparison");
        for(auto& argument:options.arguments) if(argument.name=="--mode") argument.value="draws";
        Check(options.BuildArguments(built,error) && built=="\"--mode\" \"draws\"","Example14 individual mode uses the actual draws token");
        auto frames=Arg("--frames","4294967295",ArgumentType::Integer); frames.minimum=0; frames.maximum=UINT32_MAX;
        Check(viewer::BuildArgumentString({frames},built,error),"Unsigned32 upper boundary accepted");
        frames.value="0"; Check(viewer::BuildArgumentString({frames},built,error),"Unlimited zero frame value accepted");
        for(const auto* invalid:{"-1","4294967296","1.5","1x",""," 1"})
        { frames.value=invalid; Check(!viewer::BuildArgumentString({frames},built,error) && !error.empty(),"Invalid unsigned32 input rejected"); }
        for(const auto* invalid:{"nan","inf","1e9999","1.2x",""})
            Check(!viewer::BuildArgumentString({Arg("--float",invalid,ArgumentType::Float)},built,error),"Nonfinite/malformed float rejected");
        Check(!viewer::BuildArgumentString({Arg("--int","9223372036854775808",ArgumentType::Integer)},built,error),"Signed integer overflow rejected");
        auto enumeration=Arg("--mode","bad",ArgumentType::Choice); enumeration.choices={"draws","instanced"};
        Check(!viewer::BuildArgumentString({enumeration},built,error),"Unknown enum choice rejected");
        Check(viewer::BuildArgumentString({Arg("--frames=","120",ArgumentType::Integer)},built,error),"Equals-style numeric option supported");
        auto equalsCommand=L"test.exe "+viewer::ToWide(built); int equalsCount=0;
        auto** equalsArgs=CommandLineToArgvW(equalsCommand.c_str(),&equalsCount);
        const bool joined=equalsArgs && equalsCount==2 && std::wstring(equalsArgs[1])==L"--frames=120";
        if(equalsArgs) LocalFree(equalsArgs); Check(joined,"Equals-style option remains exactly one token");
        options.rawArguments="stale"; options.arguments.push_back(Arg("--stale","old",ArgumentType::Text)); options.useRaw=true;
        options.SelectExecutable(Utf8(root/L"AdvancedResourceReuse.exe"));
        Check(options.known && !options.useRaw && options.rawArguments.empty()
            && std::none_of(options.arguments.begin(),options.arguments.end(),[](const auto& arg){return arg.name=="--stale";}),
            "Changing executable clears prior arguments and raw command");
        options.SelectExecutable(Utf8(root/L"unknown.exe"));
        Check(!options.known && options.arguments.empty(),"Unknown executable receives no guessed options");
        options.SelectExecutable(Utf8(root/L"RhiPrimitives.exe"));
        Check(options.known && options.arguments.empty(),"A known main() without argc/argv receives no fake common options");
        options.SelectExecutable(Utf8(root/L"AdvancedGPUCulling.exe"));
        Check(options.known && options.arguments.size()==1 && options.arguments.front().name=="--help",
            "Guidance executables expose only their actual standalone help flag");
        options.SelectExecutable(Utf8(root/L"AdvancedPBRLighting.exe"));
        for(auto& argument:options.arguments) if(argument.name=="--roughness") argument.value="0.001";
        Check(!options.BuildArguments(built,error),"Known float ranges reject values outside the source contract");
        options.SelectExecutable(Utf8(root/L"AdvancedDepthSampling.exe"));
        for(auto& argument:options.arguments) if(argument.name=="--far") argument.value="0.01";
        Check(!options.BuildArguments(built,error),"Near/far validation checks their relationship");
        for(auto& argument:options.arguments) if(argument.name=="--help") argument.enabled=true;
        Check(options.BuildArguments(built,error) && built=="\"--help\"","Explicit known help launches only the supported help token");
        for(const auto* executable:{L"AdvancedDepthReverseZ.exe",L"AdvancedDepthSampling.exe"})
        {
            options.SelectExecutable(Utf8(root/executable));
            for(auto& argument:options.arguments) {if(argument.name=="--near") argument.value="1"; if(argument.name=="--far") argument.value="1.00000001";}
            Check(!options.BuildArguments(built,error),"Near/far values equal after actual float rounding must be rejected");
            for(auto& argument:options.arguments) {if(argument.name=="--near") argument.value="1e-100"; if(argument.name=="--far") argument.value="2";}
            Check(!options.BuildArguments(built,error),"Actual float parser underflow must be rejected");
            for(auto& argument:options.arguments) if(argument.name=="--near") argument.value="1e39";
            Check(!options.BuildArguments(built,error),"Actual float parser overflow must be rejected");
            for(auto& argument:options.arguments) {if(argument.name=="--near") argument.value="1"; if(argument.name=="--far") argument.value="1.000001";}
            Check(options.BuildArguments(built,error),"Near/far values that remain distinct as floats must be accepted");
        }
        options.SelectExecutable(Utf8(root/L"RhiImGui.exe"));
        for(auto& argument:options.arguments) { if(argument.name=="--self-test" || argument.name=="--baseline") argument.enabled=true; }
        Check(!options.BuildArguments(built,error),"Source-specific mutually exclusive switches are rejected");
        for(auto& argument:options.arguments) { if(argument.name=="--baseline") argument.enabled=false; if(argument.name=="--frames=") {argument.enabled=true;argument.value="3";} }
        Check(!options.BuildArguments(built,error),"ImGui self-test frame minimum is enforced");
        for(auto& argument:options.arguments) if(argument.name=="--frames=") argument.value="120";
        Check(options.BuildArguments(built,error) && built.find("--frames=120")!=built.npos,"ImGui uses its actual equals syntax");

        options.SelectExecutable(Utf8(root/L"LiveExternalConsumer.exe"));
        Check(options.known && options.BuildArguments(built,error) && built=="\"--features\"",
            "LiveExternalConsumer defaults to its real features flag without finite frames or automatic verification");
        const auto consumerArgument=[&](const char* name)->LaunchArgument& {
            const auto found=std::find_if(options.arguments.begin(),options.arguments.end(),[&](const auto& value){return value.name==name;});
            if(found==options.arguments.end()) throw std::runtime_error("Missing consumer source option");
            return *found;
        };
        Check(options.arguments.size()==6 && consumerArgument("--features").type==ArgumentType::Flag
            && !consumerArgument("--verify").enabled && !consumerArgument("--frames").enabled
            && consumerArgument("--frames").minimum==0 && consumerArgument("--frames").maximum==UINT32_MAX,
            "Consumer exposes exactly its six supported options and actual unsigned32/unlimited frame contract");
        for(const auto* value:{"0","1","4294967295"})
        {
            auto& count=consumerArgument("--frames"); count.enabled=true; count.value=value;
            Check(options.BuildArguments(built,error),"Consumer accepts unsigned32 frame boundaries including unlimited zero");
        }
        for(const auto* value:{"-1","4294967296","1x",""," 1"})
        {
            consumerArgument("--frames").value=value;
            Check(!options.BuildArguments(built,error) && !error.empty(),"Consumer rejects values its unsigned32 parser cannot consume");
        }
        consumerArgument("--frames").enabled=false;
        for(int order=0;order<2;++order)
        {
            if(order) std::reverse(options.arguments.begin(),options.arguments.end());
            for(unsigned mask=0;mask<16;++mask)
            {
                consumerArgument("--features").enabled=(mask&1)!=0; consumerArgument("--raw").enabled=(mask&2)!=0;
                consumerArgument("--renderer").enabled=(mask&4)!=0; consumerArgument("--canvas").enabled=(mask&8)!=0;
                const bool accepted=!(bool(mask&1) && bool(mask&(2|8)));
                Check(options.BuildArguments(built,error)==accepted,
                    "Actual consumer contract allows features+renderer, rejects features+raw/canvas in either order, and accepts legacy mode combinations");
            }
        }
        options.SelectExecutable(Utf8(root/L"LIVEEXTERNALCONSUMER.EXE"));
        Check(options.known && options.BuildArguments(built,error) && built=="\"--features\"","Consumer preset follows case-insensitive Windows names");
        consumerArgument("--verify").enabled=true;
        Check(options.BuildArguments(built,error) && built.find("\"--verify\"")!=built.npos,"Automatic finite verification remains an explicit optional flag");
        options.useRaw=true; options.rawArguments="--raw --features";
        Check(options.BuildArguments(built,error) && built==options.rawArguments,"Explicit raw command preserves the existing direct-string compatibility path");

        const auto child=root/L"한글 공백"/L"real child.exe";
        fs::copy_file(ownPath,child);
        options.SelectExecutable(Utf8(child));
        options.arguments={Arg("--generated-child","",ArgumentType::Flag),Arg("--cwd",Utf8(child.parent_path()),ArgumentType::Text),
            Arg("--text","한글 공백\\\"text\\",ArgumentType::Text),Arg("--empty","",ArgumentType::Text),
            Arg("--integer","-17",ArgumentType::Integer),Arg("--float","1.25e-3",ArgumentType::Float)};
        Check(options.BuildArguments(built,error),"Typed arguments serialize");
        viewer::TargetProcess process;
        Check(process.Launch(options.executable,built,options.directory,error),error.c_str());
        const auto deadline=GetTickCount64()+5000; while(process.Running() && GetTickCount64()<deadline) Sleep(5);
        Check(process.ExitCode()==61u,"Actual child receives exact quoted Unicode/empty/numeric tokens and selected parent cwd");
        Check(!process.Launch(Utf8(root/L"missing.exe"),built,options.directory,error) && !error.empty(),"Missing selected executable fails safely");
        if(argc>1 && std::wstring(argv[1])==L"--scan" && argc==3)
        {
            const auto actual=viewer::ScanExecutables(viewer::ToUtf8(argv[2]));
            Check(actual.error.empty() && !actual.errors,"Requested real catalog completes without access errors");
            std::printf("catalog_count=%zu skipped_links=%llu\n",actual.files.size(),actual.skippedLinks);
            for(const auto& file:actual.files) std::printf("catalog_relative=%s\n",file.relativePath.c_str());
        }
        std::puts("PASS recursive executable catalog, selection isolation, numeric/enum validation, equals/quoted tokens and actual child argv/cwd");
        return 0;
    }
    catch(const std::exception& error) { std::fprintf(stderr,"FAIL launcher: %s\n",error.what()); return 1; }
}
