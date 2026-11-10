include_guard(GLOBAL)

# Keep pinned dependency checkouts intact; compile checked copies in the build tree.
function(dy_replace_dependency_text variable before after)
    string(FIND "${${variable}}" "${before}" first)
    string(FIND "${${variable}}" "${before}" last REVERSE)
    if(first LESS 0 OR NOT first EQUAL last)
        message(FATAL_ERROR "Dependency input-check source changed; review the pinned-source patch")
    endif()
    string(REPLACE "${before}" "${after}" contents "${${variable}}")
    set(${variable} "${contents}" PARENT_SCOPE)
endfunction()

function(dy_write_dependency_source output contents)
    get_filename_component(output_dir "${output}" DIRECTORY)
    file(MAKE_DIRECTORY "${output_dir}")
    set(existing "")
    if(EXISTS "${output}")
        file(READ "${output}" existing)
    endif()
    if(NOT existing STREQUAL contents)
        file(WRITE "${output}" "${contents}")
    endif()
endfunction()

function(dy_use_checked_source target original output contents)
    dy_write_dependency_source("${output}" "${contents}")
    get_target_property(source_dir ${target} SOURCE_DIR)
    get_target_property(sources ${target} SOURCES)
    set(replaced 0)
    set(checked_sources "")
    foreach(source IN LISTS sources)
        get_filename_component(absolute "${source}" ABSOLUTE BASE_DIR "${source_dir}")
        if(absolute STREQUAL original)
            list(APPEND checked_sources "${output}")
            math(EXPR replaced "${replaced}+1")
        else()
            list(APPEND checked_sources "${source}")
        endif()
    endforeach()
    if(NOT replaced EQUAL 1)
        message(FATAL_ERROR "Expected exactly one dependency source to replace: ${original}")
    endif()
    set_property(TARGET ${target} PROPERTY SOURCES "${checked_sources}")
endfunction()

function(dy_check_fastgltf_inputs)
    get_target_property(source_dir fastgltf SOURCE_DIR)
    get_target_property(binary_dir fastgltf BINARY_DIR)
    set(original "${source_dir}/src/fastgltf.cpp")
    file(READ "${original}" contents)
    string(REPLACE "\r\n" "\n" contents "${contents}")
    set(escape_check [=[const auto isHex = [](char value) {
            return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f')
                || (value >= 'A' && value <= 'F');
        };
        if (x.size() - i < 3 || !isHex(x[i + 1]) || !isHex(x[i + 2])
            || (x[i + 1] == '0' && x[i + 2] == '0')) {
            x.clear();
            return;
        }

        // Read the next two chars and store them]=])
    dy_replace_dependency_text(contents "// Read the next two chars and store them" "${escape_check}")
    dy_replace_dependency_text(contents
        "view = other.view;\n\t_scheme = other._scheme;"
        "view = other.view;\n\t_valid = other._valid;\n\t_scheme = other._scheme;")
    dy_replace_dependency_text(contents
        "void fg::URI::readjustViews(const URIView& other) {"
        "void fg::URI::readjustViews(const URIView& other) {\n\tview = URIView();\n\tview._valid = other._valid;")
    dy_replace_dependency_text(contents
        "void fg::URIView::parse() {"
        "void fg::URIView::parse() {\n\t_valid = true;\n\t_scheme = _path = _userinfo = _host = _port = _query = _fragment = {};")
    dy_replace_dependency_text(contents
        "fg::URI& fg::URI::operator=(const URI& other) {"
        "fg::URI& fg::URI::operator=(const URI& other) {\n\tif (this == &other) return *this;")
    dy_replace_dependency_text(contents
        "fg::URI& fg::URI::operator=(URI&& other) noexcept {"
        "fg::URI& fg::URI::operator=(URI&& other) noexcept {\n\tif (this == &other) return *this;")
    dy_replace_dependency_text(contents "\tother.view._valid = false;" "")
    dy_replace_dependency_text(contents
        "\t\t// No reallocation happened, we can safely copy the view.\n\t\tview = other.view;\n\t}\n\treturn *this;"
        "\t\t// No reallocation happened, we can safely copy the view.\n\t\tview = other.view;\n\t}\n\tother.view._valid = false;\n\treturn *this;")
    dy_use_checked_source(fastgltf "${original}" "${binary_dir}/input-checks/fastgltf.cpp" "${contents}")

    set(original "${source_dir}/src/io.cpp")
    file(READ "${original}" contents)
    string(REPLACE "\r\n" "\n" contents "${contents}")
    dy_replace_dependency_text(contents
        "URI decodedUri(uri.path()); // Re-allocate so we can decode potential characters."
        "URI decodedUri(uri.path()); // Re-allocate so we can decode potential characters.\n\tif (!decodedUri.valid() || !decodedUri.isLocalPath()) return Error::InvalidURI;")
    dy_use_checked_source(fastgltf "${original}" "${binary_dir}/input-checks/io.cpp" "${contents}")
endfunction()

function(dy_check_tracy_inputs)
    get_target_property(source_dir TracyClient SOURCE_DIR)
    get_target_property(binary_dir TracyClient BINARY_DIR)
    set(output_dir "${binary_dir}/input-checks/public")
    file(COPY "${source_dir}/public/" DESTINATION "${output_dir}"
        PATTERN "TracyProfiler.cpp" EXCLUDE PATTERN "TracyProfiler.hpp" EXCLUDE)
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/Extends/Tracy/QueryTransferBounds.h"
        "${output_dir}/client/DyfQueryTransferBounds.h" COPYONLY)

    file(READ "${source_dir}/public/client/TracyProfiler.hpp" header)
    string(REPLACE "\r\n" "\n" header "${header}")
    dy_replace_dependency_text(header
        "    char* m_queryDataPtr;"
        "    char* m_queryDataPtr = nullptr;\n    dyf::Detail::QueryTransferBounds m_queryDataBounds;\n    void ResetQueryData();")
    set(header "#include \"DyfQueryTransferBounds.h\"\n${header}")
    dy_write_dependency_source("${output_dir}/client/TracyProfiler.hpp" "${header}")

    file(READ "${source_dir}/public/client/TracyProfiler.cpp" contents)
    string(REPLACE "\r\n" "\n" contents "${contents}")
    set(reset [=[void Profiler::ResetQueryData()
{
    tracy_free( m_queryData );
    tracy_free( m_queryImage );
    m_queryData = m_queryDataPtr = m_queryImage = nullptr;
    m_queryDataBounds = {};
}

bool Profiler::HandleServerQuery()]=])
    dy_replace_dependency_text(contents "bool Profiler::HandleServerQuery()" "${reset}")
    dy_replace_dependency_text(contents
        "    m_queryData = nullptr;\n    m_queryImage = nullptr;"
        "    m_queryData = nullptr;\n    m_queryImage = nullptr;\n    m_queryDataPtr = nullptr;\n    m_queryDataBounds = {};")
    set(before [=[    case ServerQuerySourceCode:
        QueueSourceCodeQuery( uint32_t( ptr ) );
        break;
    case ServerQueryDataTransfer:
        if( m_queryData )
        {
            assert( !m_queryImage );
            m_queryImage = m_queryData;
        }
        m_queryDataPtr = m_queryData = (char*)tracy_malloc( ptr + 11 );
        AckServerQuery();
        break;
    case ServerQueryDataTransferPart:
        memcpy( m_queryDataPtr, &ptr, 8 );]=])
    set(after [=[    case ServerQuerySourceCode:
        if( m_exectime == 0 || !m_queryData || !m_queryDataBounds.Complete()
            || m_queryData[m_queryDataBounds.length - 1] != '\0' ) return false;
        QueueSourceCodeQuery( uint32_t( ptr ) );
        break;
    case ServerQueryDataTransfer:
        {
            dyf::Detail::QueryTransferBounds bounds;
            if( !bounds.Begin( ptr ) || (m_queryData && (m_queryImage
                || !m_queryDataBounds.Complete()
                || m_queryData[m_queryDataBounds.length - 1] != '\0')) ) return false;
            auto data = (char*)tracy_malloc( bounds.remaining );
            if( !data ) return false;
            if( m_queryData ) m_queryImage = m_queryData;
            m_queryDataPtr = m_queryData = data;
            m_queryDataBounds = bounds;
        }
        AckServerQuery();
        break;
    case ServerQueryDataTransferPart:
        if( !m_queryData || !m_queryDataBounds.ConsumePart() ) return false;
        memcpy( m_queryDataPtr, &ptr, 8 );]=])
    dy_replace_dependency_text(contents "${before}" "${after}")
    # All socket ownership releases end the connection's partial transfer.
    string(FIND "${contents}" "m_sock = nullptr;" socket_release)
    if(socket_release LESS 0)
        message(FATAL_ERROR "Tracy connection cleanup changed; review the input-check patch")
    endif()
    string(REPLACE "m_sock = nullptr;" "ResetQueryData();\n        m_sock = nullptr;" contents "${contents}")
    dy_replace_dependency_text(contents
        "    tracy_free( s_thread );"
        "    tracy_free( s_thread );\n    ResetQueryData();")
    dy_write_dependency_source("${output_dir}/client/TracyProfiler.cpp" "${contents}")
    file(READ "${source_dir}/public/TracyClient.cpp" client)
    dy_use_checked_source(TracyClient "${source_dir}/public/TracyClient.cpp"
        "${output_dir}/TracyClient.cpp" "${client}")
    target_include_directories(TracyClient BEFORE PUBLIC "$<BUILD_INTERFACE:${output_dir}>")
    include(GNUInstallDirs)
    install(FILES "${output_dir}/client/TracyProfiler.hpp"
        "${output_dir}/client/DyfQueryTransferBounds.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/tracy/client")
endfunction()
