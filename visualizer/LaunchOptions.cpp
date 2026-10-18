#include "LaunchOptions.h"
#include "Process.h"
#include <windows.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <stdexcept>

namespace viewer
{
namespace fs=std::filesystem;
static std::string PathText(const fs::path& path)
{
    try { return ToUtf8(path.native()); } catch(...) { return "[Unicode path conversion failed]"; }
}
ExecutableScan ScanExecutables(const std::string& folder,const std::atomic_bool* cancel)
{
    ExecutableScan result;
    const auto cancelled=[&] { return cancel && cancel->load(std::memory_order_relaxed); };
    const auto issue=[&](const fs::path& path,const std::string& reason) {
        if(result.issues.size()<32) result.issues.push_back(PathText(path)+": "+reason);
    };
    try
    {
        if(folder.empty()) throw std::invalid_argument("검색할 폴더를 지정하세요.");
        const auto root=fs::absolute(fs::path(ToWide(folder))).lexically_normal();
        result.root=ToUtf8(root.native());
        std::error_code error;
        if(!fs::is_directory(root,error)) throw std::invalid_argument("폴더가 없거나 읽을 수 없습니다: "+result.root+(error ? " ("+error.message()+")" : ""));
        std::vector<fs::path> pending{root};
        while(!pending.empty() && !cancelled())
        {
            const auto current=std::move(pending.back()); pending.pop_back();
            fs::directory_iterator entry(current,error),end;
            if(error) { ++result.errors; issue(current,error.message()); continue; }
            while(entry!=end && !cancelled())
            {
                const auto path=entry->path();
                const auto attributes=GetFileAttributesW(path.c_str());
                if(attributes==INVALID_FILE_ATTRIBUTES)
                { ++result.errors; issue(path,"Win32 "+std::to_string(GetLastError())); }
                else if(attributes&FILE_ATTRIBUTE_DIRECTORY)
                {
                    if(attributes&FILE_ATTRIBUTE_REPARSE_POINT)
                    { ++result.skippedLinks; issue(path,"링크/junction 폴더 생략 (순환 방지)"); }
                    else pending.push_back(path);
                }
                else if(_wcsicmp(path.extension().c_str(),L".exe")==0)
                {
                    if(entry->is_regular_file(error))
                    {
                        try { result.files.push_back({ToUtf8(path.native()),ToUtf8(path.lexically_relative(root).generic_wstring())}); }
                        catch(const std::exception& exception) { ++result.errors; issue(path,exception.what()); }
                    }
                    else if(error) { ++result.errors; issue(path,error.message()); }
                }
                entry.increment(error);
                if(error) { ++result.errors; issue(current,error.message()); break; }
            }
        }
        result.cancelled=cancelled();
        std::sort(result.files.begin(),result.files.end(),[](const auto& a,const auto& b){return a.relativePath<b.relativePath;});
    }
    catch(const std::exception& exception) { result.error=exception.what(); }
    return result;
}
bool BuildArgumentString(const std::vector<LaunchArgument>& values,std::string& arguments,std::string& error)
{
    arguments.clear(); error.clear();
    try
    {
        std::wstring command;
        const auto append=[&](const std::string& value) {
            if(!command.empty()) command+=L' ';
            command+=QuoteArgument(ToWide(value));
        };
        for(const auto& argument:values) if(argument.enabled)
        {
            if(argument.name.empty() || argument.name.find_first_of(" \t\r\n\"")!=std::string::npos)
                throw std::invalid_argument("공백 없는 인자 이름을 입력하세요.");
            if(argument.type==ArgumentType::Integer)
            {
                int64_t value=0; const auto parsed=std::from_chars(argument.value.data(),argument.value.data()+argument.value.size(),value);
                if(parsed.ec!=std::errc{} || parsed.ptr!=argument.value.data()+argument.value.size() || value<argument.minimum || value>argument.maximum)
                    throw std::invalid_argument(argument.name+": 정수 범위 "+std::to_string(argument.minimum)+" ~ "+std::to_string(argument.maximum));
            }
            else if(argument.type==ArgumentType::Float)
            {
                double value=0; const auto parsed=std::from_chars(argument.value.data(),argument.value.data()+argument.value.size(),value);
                if(parsed.ec!=std::errc{} || parsed.ptr!=argument.value.data()+argument.value.size() || !std::isfinite(value)
                    || value<argument.minimumFloat || value>argument.maximumFloat || (argument.exclusiveMinimum && value==argument.minimumFloat))
                    throw std::invalid_argument(argument.name+": 허용 범위의 유한한 실수를 입력하세요. "+argument.description);
            }
            else if(argument.type==ArgumentType::Choice && std::find(argument.choices.begin(),argument.choices.end(),argument.value)==argument.choices.end())
                throw std::invalid_argument(argument.name+": 목록에서 값을 선택하세요.");
            if(argument.type!=ArgumentType::Flag && argument.name.back()=='=') append(argument.name+argument.value);
            else { append(argument.name); if(argument.type!=ArgumentType::Flag) append(argument.value); }
        }
        if(command.size()>=32767) throw std::invalid_argument("Windows 인자 길이 한도를 초과했습니다.");
        arguments=ToUtf8(command); return true;
    }
    catch(const std::exception& exception) { error=exception.what(); return false; }
}
static LaunchArgument Option(std::string name,std::string value,ArgumentType type,std::string description,bool enabled=true)
{
    LaunchArgument result; result.name=std::move(name); result.value=std::move(value); result.type=type;
    result.description=std::move(description); result.enabled=enabled; return result;
}
static LaunchArgument Frames(const char* name="--frames")
{
    auto value=Option(name,"0",ArgumentType::Integer,"실행 frame 수 (0 = 창을 닫을 때까지)");
    value.minimum=0; value.maximum=UINT32_MAX; value.enabled=false; return value;
}
static LaunchArgument Integer(const char* name,const char* value,int64_t minimum,int64_t maximum,const char* description)
{ auto result=Option(name,value,ArgumentType::Integer,description); result.minimum=minimum; result.maximum=maximum; return result; }
static LaunchArgument Real(const char* name,const char* value,double minimum,double maximum,const char* description,bool enabled=true)
{ auto result=Option(name,value,ArgumentType::Float,description,enabled); result.minimumFloat=minimum; result.maximumFloat=maximum; return result; }
static LaunchArgument Choice(const char* name,std::vector<std::string> choices,const char* description)
{ auto result=Option(name,choices.front(),ArgumentType::Choice,description); result.choices=std::move(choices); return result; }
void LaunchOptions::SelectExecutable(const std::string& path)
{
    LaunchOptions next;
    if(path.empty()) { *this=std::move(next); return; }
    const auto absolute=fs::absolute(fs::path(ToWide(path))).lexically_normal();
    next.executable=ToUtf8(absolute.native()); next.directory=ToUtf8(absolute.parent_path().native());
    const auto name=absolute.filename().native();
    const wchar_t* advanced[]={L"Profiling",L"DepthReverseZ",L"DepthSampling",L"StencilMask",L"ResourceBarriers",L"FramesAndPresent",
        L"PBRLighting",L"ShadowMapping",L"ClusteredLighting",L"DeferredLighting",L"HDRPostProcess",L"TemporalAA",L"VolumetricFog",
        L"InstancingBatching",L"ResourceReuse",L"BindlessMaterials",L"IndirectDrawing",L"GPUCulling",L"MultithreadRecording",L"AsyncCompute",
        L"RenderGraph",L"MeshShaderBasics",L"MeshletCullingLOD",L"AccelerationStructures",L"RayQueryShadows",L"RayTracingReflections",L"RayTracingDenoising",L"HybridRendering"};
    int example=0;
    for(size_t i=0;i<std::size(advanced);++i) if(_wcsicmp(name.c_str(),(std::wstring(L"Advanced")+advanced[i]+L".exe").c_str())==0) example=int(i)+1;
    if(example)
    {
        next.known=true;
        next.source="examples/advanced/"+(example<10 ? std::string("0") : std::string())+std::to_string(example)+"_"+ToUtf8(advanced[example-1])+"/main.cpp";
        const auto flag=[&](const char* key,const char* description){next.arguments.push_back(Option(key,"",ArgumentType::Flag,description,false));};
        const auto mode=[&](std::vector<std::string> choices){next.arguments.push_back(Choice("--mode",std::move(choices),"실행 비교 / 표시 모드"));};
        const auto capture=[&] { next.arguments.push_back(Option("--capture","image.ppm",ArgumentType::Text,"PPM 캡처. 08/10/12는 frames 생략 시 유한 실행, 명시적 0은 불가. 14/16은 자동 종료 없음.",false)); };
        const auto flight=[&] { next.arguments.push_back(Integer("--flight","2",1,4,"동시 frame 수 1~4")); };
        const auto exposure=[&] { next.arguments.push_back(Real("--exposure","1",.01,20,"노출 0.01~20")); };
        const auto surface=[&] { next.arguments.push_back(Real("--roughness","0.45",.045,1,"거칠기 0.045~1")); next.arguments.push_back(Real("--metallic","0.8",0,1,"금속성 0~1")); };
        const bool helpOnly=example==17 || example==18 || example==20 || example>=23;
        if(!helpOnly)
        {
            auto frames=Frames(); if(example>=7 && example<=13) frames.maximum=10000000;
            if(example==19) {frames.minimum=1;frames.value="3";frames.description="실행 frame 수 1~4294967295 (기본 3)";}
            next.arguments.push_back(frames);
            flag("--validation","backend validation 활성화");
            if(example>=7 && example<=13) next.arguments.push_back(Real("--time","0",0,1000000,"고정 애니메이션 시간 0~1000000초 (생략 시 시간 진행)",false));
        }
        flag("--help",helpOnly ? "단독 도움말. 인자 없으면 지원 조회 후 안내 종료 (77)." : "도움말 출력 후 종료");
        switch(example)
        {
        case 1: flight(); next.arguments.push_back(Integer("--work","96",0,2048,"픽셀 셰이더 부하 0~2048")); break;
        case 2: case 3:
        {
            auto nearPlane=Real("--near","0.1",0,std::numeric_limits<float>::max(),"near > 0, far > near"); nearPlane.exclusiveMinimum=true;
            next.arguments.push_back(nearPlane); next.arguments.push_back(Real("--far",example==2 ? "100000" : "100",0,std::numeric_limits<float>::max(),"far > near"));
            if(example==2) {flag("--reverse","Reverse-Z");flag("--no-depth-test","깊이 테스트 비활성화");flag("--no-depth-write","깊이 기록 비활성화");capture();}
            else {mode({"default","split","raw","linear"});flag("--wait","프로듀서 제출 후 GPU 대기");}
            break;
        }
        case 4: mode({"mask","outline","default"}); next.arguments.push_back(Integer("--reference","1",0,255,"스텐실 비교값 0~255")); capture(); break;
        case 5: flight(); flag("--wait","프로듀서별 제출 / 대기"); break;
        case 6: flight(); mode({"fifo","immediate","mailbox","default"}); capture(); break;
        case 7: mode({"pbr","lambert","normal"}); surface(); exposure(); break;
        case 8:
            mode({"shadow","unshadowed","depth","visibility","regions"}); next.arguments.push_back(Choice("--shadow-type",{"directional","cascaded","point"},"그림자 종류"));
            next.arguments.push_back(Real("--bias","0.0015",0,.1,"bias 0~0.1")); next.arguments.push_back(Integer("--pcf","1",0,2,"PCF 범위 0~2"));
            next.arguments.push_back(Real("--split","8",4,16,"cascade split 4~16")); next.arguments.push_back(Real("--light-height","4",2,8,"조명 높이 2~8")); exposure(); capture(); break;
        case 9: mode({"clustered","all","count"}); next.arguments.push_back(Integer("--lights","64",1,64,"조명 개수 1~64")); exposure(); break;
        case 10: mode({"lit","normal","albedo","position"}); next.arguments.push_back(Integer("--lights","24",0,64,"조명 개수 0~64")); surface(); exposure(); capture(); break;
        case 11: mode({"aces","reinhard","clamp","no-bloom","bloom"}); next.arguments.push_back(Real("--threshold","1",0,20,"bloom threshold 0~20"));
            next.arguments.push_back(Real("--bloom","0.35",0,5,"bloom 강도 0~5")); next.arguments.push_back(Real("--emission","12",1,100,"발광 1~100")); exposure(); break;
        case 12: mode({"taa","off","unclamped","velocity"}); next.arguments.push_back(Real("--history-weight","0.9",0,.99,"history 가중치 0~0.99")); capture(); break;
        case 13: mode({"fog","off","transmittance","scattering"}); next.arguments.push_back(Real("--density","0.22",0,3,"밀도 0~3"));
            next.arguments.push_back(Integer("--steps","64",8,256,"raymarch 단계 8~256")); next.arguments.push_back(Real("--height-falloff","0.7",0,4,"높이 감쇠 0~4")); exposure(); break;
        case 14:
            mode({"instanced","draws"}); capture();
            next.description="대표 예제 14: 같은 256개 인스턴스·같은 출력을 ON=Draw 1 / OFF=Draw 256으로 비교합니다. Controls에서 인스턴싱을 선택해 적용 요청하세요. --features는 불필요합니다. FIFO의 FPS 상한을 고려해 CPU 기록·Draw/Batch 변화를 먼저 보세요.";
            break;
        case 15: mode({"reuse","create"}); break;
        case 16: mode({"array","material"}); capture(); break;
        case 19: mode({"parallel","serial"}); next.arguments.push_back(Integer("--workers","2",0,64,"worker 0=하드웨어 동시성, 1=호출 스레드, 최대 64")); capture(); break;
        case 21: mode({"graph","manual"}); break;
        default: break;
        }
    }
    const wchar_t* regular[]={L"Primitives",L"Image",L"Viewport",L"GeometryCamera",L"MeshMaterial",L"Light",L"ModelAnim",L"Scene",L"Text"};
    for(const auto* prefix:{L"Rhi",L"Dyf"}) for(const auto* suffix:regular)
        if(_wcsicmp(name.c_str(),(std::wstring(prefix)+suffix+L".exe").c_str())==0)
        { next.known=true; next.source="examples/"+std::string(_wcsicmp(prefix,L"Rhi")==0 ? "RHI" : "dyf")+"/*/main.cpp: main(), CLI 파서 없음"; }
    const bool rhiGui=_wcsicmp(name.c_str(),L"RhiImGui.exe")==0;
    if(rhiGui || _wcsicmp(name.c_str(),L"DyEditor.exe")==0)
    {
        next.known=true; next.source=rhiGui ? "examples/RHI/10_ImGui/main.cpp" : "examples/dyf/10_ImGui/main.cpp";
        next.arguments.push_back(Option("--self-test","",ArgumentType::Flag,rhiGui ? "최소 100 frame, baseline과 상호 배타" : "최소 32 frame",false));
        if(rhiGui) next.arguments.push_back(Option("--baseline","",ArgumentType::Flag,"GUI 없는 비교 (self-test와 상호 배타)",false));
        auto frames=Frames("--frames="); frames.minimum=1; frames.maximum=INT32_MAX; frames.value=rhiGui ? "120" : "60"; frames.description="양의 int32. 생략 시 무제한; capture/self-test는 자체 기본 frame 수 사용.";
        next.arguments.push_back(frames); next.arguments.push_back(Option("--capture=","image.ppm",ArgumentType::Text,"PPM 캡처. 단독 지정 시 RHI 120 / Editor 60 frame",false));
        if(!rhiGui) next.arguments.push_back(Option("--project=","EditorProject",ArgumentType::Text,"프로젝트 폴더 (생략 시 cwd/EditorProject)",false));
    }
    if(_wcsicmp(name.c_str(),L"LiveExternalConsumer.exe")==0)
    {
        next.known=true; next.source="tests/Fixtures/LiveConsumer/main.cpp: ParseOptions()";
        next.description="인자 없음 = 최소 RHI 검증. 기본 --features = Renderer + Canvas + RHI 최적화 비교. Model은 별도 Model 제작물이 필요합니다. raw/renderer/canvas는 명령 미리보기의 마지막 모드가 적용됩니다.";
        next.arguments={
            Option("--features","",ArgumentType::Flag,"Renderer + Canvas + RHI 비교. --renderer와 함께 가능하며 --raw/--canvas와는 함께 사용할 수 없습니다."),
            Option("--raw","",ArgumentType::Flag,"최소 RHI 검증 (--features를 끄고 선택)",false),
            Option("--renderer","",ArgumentType::Flag,"기존 Renderer + Canvas 경로. --features와 함께 사용할 수 있습니다.",false),
            Option("--canvas","",ArgumentType::Flag,"Canvas 경로 (--features를 끄고 선택)",false),
            Option("--verify","",ArgumentType::Flag,"자동 검증 후 종료: features 9, renderer 16, raw/canvas 8 frames. --frames 값을 덮어씁니다.",false),Frames()};
    }
    *this=std::move(next);
}
bool LaunchOptions::BuildArguments(std::string& result,std::string& error) const
{
    error.clear(); result.clear();
    try
    {
        const auto find=[&](const char* name)->const LaunchArgument* { for(const auto& argument:arguments) if(argument.enabled && argument.name==name) return &argument; return nullptr; };
        const auto* help=(!useRaw && known) ? find("--help") : nullptr;
        if(useRaw) { (void)ToWide(rawArguments); result=rawArguments; }
        else if(help) { if(!BuildArgumentString({*help},result,error)) return false; }
        else if(!BuildArgumentString(arguments,result,error)) return false;
        if(!useRaw && known && !help)
        {
            const auto number=[](const LaunchArgument* value,double fallback) { if(!value) return fallback; double parsed=0; std::from_chars(value->value.data(),value->value.data()+value->value.size(),parsed); return parsed; };
            const auto filename=fs::path(ToWide(executable)).filename().native();
            const auto is=[&](const wchar_t* name){return _wcsicmp(filename.c_str(),name)==0;};
            if(is(L"LiveExternalConsumer.exe") && find("--features") && (find("--raw") || find("--canvas")))
                throw std::invalid_argument("--features는 --raw/--canvas와 함께 사용할 수 없습니다. 해당 경로를 실행하려면 --features를 끄세요.");
            if(is(L"AdvancedDepthReverseZ.exe") || is(L"AdvancedDepthSampling.exe"))
            {
                const auto* nearOption=find("--near"); const auto* farOption=find("--far");
                const float nearPlane=nearOption ? std::stof(nearOption->value) : .1f;
                const float farPlane=farOption ? std::stof(farOption->value) : is(L"AdvancedDepthReverseZ.exe") ? 100000.f : 100.f;
                if(!std::isfinite(nearPlane) || !std::isfinite(farPlane) || nearPlane<=0 || farPlane<=nearPlane)
                    throw std::invalid_argument("float 정밀도에서 0 < near < far여야 합니다.");
            }
            if(find("--capture") && find("--frames") && number(find("--frames"),0)==0
                && (is(L"AdvancedShadowMapping.exe") || is(L"AdvancedDeferredLighting.exe") || is(L"AdvancedTemporalAA.exe")))
                throw std::invalid_argument("이 예제는 capture와 명시적 --frames 0을 함께 사용할 수 없습니다. frame 수를 지정하거나 frames 항목을 끄세요.");
            if(find("--self-test"))
            {
                if(find("--baseline")) throw std::invalid_argument("--self-test와 --baseline은 함께 사용할 수 없습니다.");
                const auto minimum=is(L"RhiImGui.exe") ? 100 : 32;
                if(find("--frames=") && number(find("--frames="),minimum)<minimum) throw std::invalid_argument("self-test에 필요한 최소 frame 수: "+std::to_string(minimum));
            }
        }
        const auto command=QuoteArgument(ToWide(executable))+(result.empty() ? L"" : L" "+ToWide(result));
        if(command.size()>=32767) throw std::invalid_argument("Windows 명령줄 길이 한도를 초과했습니다.");
        return true;
    }
    catch(const std::exception& exception) { error=exception.what(); return false; }
}
}
