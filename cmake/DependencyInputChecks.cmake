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
