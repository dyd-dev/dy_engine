if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED SYMBOL)
    message(FATAL_ERROR "INPUT, OUTPUT and SYMBOL are required")
endif()

file(READ "${INPUT}" binary HEX)
if(binary STREQUAL "")
    message(FATAL_ERROR "Cannot embed empty file: ${INPUT}")
endif()

string(REGEX REPLACE "([0-9A-Fa-f][0-9A-Fa-f])" "0x\\1," bytes "${binary}")
get_filename_component(output_directory "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")
if(NOT DEFINED NAMESPACE)
    set(NAMESPACE "dy::Graphics::Private")
endif()
file(WRITE "${OUTPUT}"
    "#pragma once\n#include <cstddef>\n#include <cstdint>\n\nnamespace ${NAMESPACE}\n{\ninline constexpr uint8_t ${SYMBOL}[] = {${bytes}};\ninline constexpr std::size_t ${SYMBOL}Size = sizeof(${SYMBOL});\n}\n")
if(DEFINED ENTRY_POINT)
    file(APPEND "${OUTPUT}"
        "namespace ${NAMESPACE} { inline constexpr const char* ${SYMBOL}EntryPoint = \"${ENTRY_POINT}\"; }\n")
endif()
