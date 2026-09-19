"""Build/run a real add_subdirectory consumer and reject CI/example leakage."""
import argparse
from pathlib import Path

import ci


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ci.ROOT)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--dependencies', type=Path, required=True)
    args = parser.parse_args()
    root, directory = args.root.resolve(), args.build_dir.resolve()
    source = directory / 'source'
    source.mkdir(parents=True, exist_ok=True)
    cmake = '''cmake_minimum_required(VERSION 3.20)
project(Consumer CXX)
add_executable(consumer main.cpp)
add_subdirectory("@ROOT@" engine)
target_link_libraries(consumer PRIVATE dy_engine)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/consumer-$<CONFIG>.txt" CONTENT "$<TARGET_FILE:consumer>")
function(check_directory directory)
    get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        if(target MATCHES "^Ci")
            message(FATAL_ERROR "Framework check target leaked: ${target}")
        endif()
        foreach(property COMPILE_OPTIONS COMPILE_DEFINITIONS LINK_OPTIONS
                         INTERFACE_COMPILE_OPTIONS INTERFACE_COMPILE_DEFINITIONS
                         INTERFACE_LINK_OPTIONS CXX_COMPILER_LAUNCHER)
            get_target_property(value "${target}" "${property}")
            if(value MATCHES "DY_CI_|sanitize|instrument.py|finstrument-functions")
                message(FATAL_ERROR "CI instrumentation leaked: ${target} ${property}=${value}")
            endif()
        endforeach()
    endforeach()
    get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(child IN LISTS children)
        if(child STREQUAL "@ROOT@/examples" OR child STREQUAL "@ROOT@/.github/ci")
            message(FATAL_ERROR "Framework examples/checks auto-registered: ${child}")
        endif()
        check_directory("${child}")
    endforeach()
endfunction()
check_directory("${CMAKE_SOURCE_DIR}")
get_target_property(includes Engine_Options INTERFACE_INCLUDE_DIRECTORIES)
if(NOT "@ROOT@/src/Public" IN_LIST includes)
    message(FATAL_ERROR "Engine public include directory resolves to consumer project")
endif()
file(WRITE "${CMAKE_BINARY_DIR}/consumer-isolation.txt"
     "PASS: no framework examples, check targets or CI instrumentation; engine headers resolve correctly.\\n")
'''.replace('@ROOT@', root.as_posix())
    (source / 'CMakeLists.txt').write_text(cmake, encoding='utf-8')
    (source / 'main.cpp').write_text('''#include "dyf/Scene.h"
#include <iostream>
int main()
{
    dyf::EntityHandle entity;
    {
        dyf::Scene scene;
        entity = scene.Add(dyf::CreateCubeMesh());
        if (!entity || scene.Materials().size() != 1 || scene.Meshes().size() != 1
            || scene.GetEntityCount() != 1 || !entity.SetPosition({1, 2, 3})
            || scene.GetEntity(dyf::EntityID::Invalid)) return 1;
    }
    if (entity) return 1;
    std::cout << "DY_CONSUMER_PASS materials=1 entities=1 expired_handle=ok\\n";
    return 0;
}
''', encoding='utf-8')
    env = ci.setup_environment(root)
    options = ['-DDY_ENABLE_TRACY=OFF', '-DUSE_VULKAN=OFF', '-DUSE_D3D12=OFF', '-DUSE_METAL=OFF']
    for name in ('glfw', 'stb'):
        dependency = args.dependencies.resolve() / (name + '-src')
        if not dependency.is_dir():
            parser.error(f'Missing dependency checkout: {dependency}')
        options.append('-DFETCHCONTENT_SOURCE_DIR_' + name.upper() + '=' + str(dependency))
    if ci.sys.platform.startswith('linux'):
        options.append('-DDY_LINUX_WINDOW_SYSTEM=X11')
    if ci.os.name == 'nt':
        options.extend(['-G', 'Visual Studio 17 2022', '-A', 'x64'])
    else:
        options.extend(['-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Debug'])
    for mode in ('default', 'inherited-ci'):
        build = directory / mode
        command = ['cmake', '-S', str(source), '-B', str(build), *options]
        if mode == 'inherited-ci':
            command.extend('-D' + flag + '=ON' for flag in
                           ('DY_CI', 'DY_CI_RUNTIME', 'DY_CI_TRACE', 'DY_CI_SANITIZERS'))
        report = ci.Report(build / 'ci-logs', 'consumer-fixture', 'null')
        try:
            report.command('configure-' + mode, command, source, env=env)
            report.command('build-' + mode, ['cmake', '--build', build, '--config', 'Debug',
                                           '--target', 'consumer', '--parallel', '2'], source, env=env)
            binary = Path((build / 'consumer-Debug.txt').read_text(encoding='utf-8'))
            output = report.command('run-' + mode, [binary], binary.parent, timeout=30, env=env)
            if 'DY_CONSUMER_PASS materials=1 entities=1 expired_handle=ok' not in output:
                raise ci.CiError('Missing consumer runtime evidence')
            evidence = build / 'consumer-isolation.txt'
            evidence.write_text(evidence.read_text(encoding='utf-8') +
                                'PASS: consumer compiled, linked dy_engine and executed Scene operations.\n', encoding='utf-8')
            print(mode + ': ' + evidence.read_text(encoding='utf-8').strip())
        finally:
            report.save()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
