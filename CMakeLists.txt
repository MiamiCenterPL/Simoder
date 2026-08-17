cmake_minimum_required(VERSION 3.24)

project(SC13ModLoader VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "SC13 Mod Loader currently targets Windows only.")
endif()

option(SC13_BUILD_RUNTIME "Build the injected DLL and launcher." ON)
option(SC13_BUILD_TESTS "Build unit tests." ON)
option(
    SC13_ENABLE_DISCOVERY_TRACE
    "Enable the retired package-I/O and executable-image discovery instrumentation."
    OFF
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

function(sc13_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(sc13_core STATIC
    src/core/patched_resource_cache.cpp
    src/core/resource_filter.cpp
    src/core/runtime_property_table.cpp
    src/core/sha256.cpp
    src/core/tgi.cpp
    src/formats/dbpf/dbpf_reader.cpp
    src/formats/dbpf/refpack.cpp
    src/formats/prop/prop_document.cpp
    src/memory/signature.cpp
    src/reverse/pe_file.cpp
)
target_include_directories(sc13_core PUBLIC src)
target_compile_definitions(sc13_core PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(sc13_core PUBLIC bcrypt)
sc13_enable_warnings(sc13_core)

add_executable(sc13-inspect tools/inspect/main.cpp)
target_link_libraries(sc13-inspect PRIVATE sc13_core)
sc13_enable_warnings(sc13-inspect)

add_executable(sc13-pe-probe tools/pe_probe/main.cpp)
target_link_libraries(sc13-pe-probe PRIVATE sc13_core)
sc13_enable_warnings(sc13-pe-probe)

add_executable(sc13-memory-probe tools/memory_probe/main.cpp)
target_link_libraries(sc13-memory-probe PRIVATE sc13_core)
sc13_enable_warnings(sc13-memory-probe)

if(SC13_BUILD_RUNTIME)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
        message(FATAL_ERROR "The SimCity 10.3.6.0 process is x86. Configure with -A Win32.")
    endif()

    include(FetchContent)
    FetchContent_Declare(
        minhook
        GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
        GIT_TAG v1.3.4
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(minhook)

    add_library(sc13modloader SHARED
        src/bootstrap/exports.def
        src/bootstrap/dll_main.cpp
        src/bootstrap/loader.cpp
        src/hooks/file_trace.cpp
        src/hooks/resource_trace.cpp
        src/logging/async_logger.cpp
        src/reverse/game_build.cpp
        src/reverse/resource_symbols.cpp
        src/runtime/patch_signal.cpp
    )
    if(SC13_ENABLE_DISCOVERY_TRACE)
        target_sources(sc13modloader PRIVATE src/runtime/image_observer.cpp)
        target_compile_definitions(sc13modloader PRIVATE SC13_ENABLE_DISCOVERY_TRACE=1)
    endif()
    target_link_libraries(sc13modloader PRIVATE sc13_core minhook version)
    target_include_directories(sc13modloader PRIVATE src)
    target_compile_definitions(sc13modloader PRIVATE SC13_RUNTIME_BUILD)
    set_target_properties(sc13modloader PROPERTIES OUTPUT_NAME "sc13modloader")
    sc13_enable_warnings(sc13modloader)

    add_executable(sc13-launcher src/bootstrap/launcher_main.cpp)
    target_link_libraries(sc13-launcher PRIVATE sc13_core)
    sc13_enable_warnings(sc13-launcher)
endif()

if(SC13_BUILD_TESTS)
    enable_testing()
    add_executable(sc13-tests tests/test_main.cpp)
    target_link_libraries(sc13-tests PRIVATE sc13_core)
    sc13_enable_warnings(sc13-tests)
    add_test(NAME sc13-tests COMMAND sc13-tests)
endif()

foreach(target sc13-inspect sc13-pe-probe sc13-memory-probe sc13modloader sc13-launcher sc13-tests)
    if(TARGET ${target})
        set_target_properties(${target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
        )
    endif()
endforeach()
