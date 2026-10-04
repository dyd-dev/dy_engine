"""Compile and execute scalar expressions from the three production shaders.

This checks AO interpolation and the signed-normal early-return predicate. It
does not emulate texture sampling, TBN construction, or a GPU shader pipeline.
"""
import argparse
import re
import subprocess
from pathlib import Path


def expressions(path):
    source = path.read_text(encoding="utf-8")
    initial = re.search(r"float occlusion\s*=\s*([^;]+);", source).group(1)
    # Skip the declaration initializer; this expression starts at its AO update.
    update = re.search(r"occlusion\s*([*]?=)\s*([^;]+);", source[source.index("float occlusion") + len("float occlusion"):])
    normal = re.search(r"if\s*\(\(textureFlags\s*&\s*(?:RENDERER_TEXTURE_FLAG_NORMAL|kTextureFlagNormal)\)\s*==\s*0u?\s*\|\|\s*([^\{]+)\)\s*\{", source).group(1)

    def translate(expression):
        expression = re.sub(r"(?:pushConstants\.|drawConstants\.)?materialParams\.w", "strength", expression)
        expression = re.sub(r"(?:pushConstants\.|drawConstants\.)?materialParams\.z", "scale", expression)
        expression = expression.replace("normalScale", "scale")
        expression = re.sub(r"SampleMaterial\(3u,[^)]*\)\.r", "sample", expression)
        expression = re.sub(r"occlusionTexture\.sample\(materialSampler,\s*input\.uv\)\.r", "sample", expression)
        expression = re.sub(r"\b(?:lerp|mix)\(", "Blend(", expression)
        expression = re.sub(r"\babs\(", "std::abs(", expression)
        expression = re.sub(r"\bclamp\(", "Clamp(", expression)
        return expression

    return translate(initial), update.group(1), translate(update.group(2)), translate(normal)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    shaders = Path(__file__).resolve().parents[1] / "src/dyf/Shaders"
    functions = []
    for language in ("hlsl", "glsl", "metal"):
        initial, operation, update, normal = expressions(shaders / ("mesh_ps." + language))
        functions.append(f"""
double AO_{language}(double strength,double sample,bool mapped) {{
    double occlusion={initial};
    if(mapped) occlusion {operation} {update};
    return occlusion;
}}
bool SkipsNormal_{language}(double scale) {{ return {normal}; }}
""")
    tests = []
    for language in ("hlsl", "glsl", "metal"):
        tests.append(f"""
    for(const auto& item:cases)
        if(std::abs(AO_{language}(item.strength,item.sample,item.mapped)-item.expected)>1e-6) {{
            std::fprintf(stderr,"FAIL {language} AO strength=%g sample=%g mapped=%d\\n",item.strength,item.sample,item.mapped);
            ++failures;
        }}
    if(SkipsNormal_{language}(-1) || SkipsNormal_{language}(1) || !SkipsNormal_{language}(0)) {{
        std::fprintf(stderr,"FAIL {language} signed normal scale\\n"); ++failures;
    }}
""")
    source = """
#include <algorithm>
#include <cmath>
#include <cstdio>
double Clamp(double a,double b,double c) { return std::clamp(a,b,c); }
double Blend(double a,double b,double t) { return a+(b-a)*t; }
""" + "".join(functions) + """
int main() {
    struct Case { double strength,sample; bool mapped; double expected; };
    const Case cases[]={{0,0,true,1},{.5,1,true,1},{.5,0,true,.5},
        {1,.25,true,.25},{.25,.25,false,1}};
    int failures=0;
""" + "".join(tests) + """
    if(!failures) std::puts("PASS shader scalar contracts: AO cases and signed normal predicate, all 3 languages");
    return failures?1:0;
}
"""
    cpp = args.output / "shader-arithmetic.cpp"
    exe = args.output / "shader-arithmetic.exe"
    cpp.write_text(source, encoding="utf-8")
    subprocess.run([args.compiler, "/nologo", "/std:c++17", "/EHsc", "/MD", str(cpp),
                    "/Fo" + str(args.output / "shader-arithmetic.obj"), "/Fe" + str(exe)], check=True)
    return subprocess.run([str(exe)]).returncode


if __name__ == "__main__":
    raise SystemExit(main())
