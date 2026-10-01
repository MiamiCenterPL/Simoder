cmake_minimum_required(VERSION 3.24)

project(SC13ModLoader VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "SC13 Mod Loader currently targets Windows only.")
endif()

option(SC13_BUILD_RUNTIME "Build the injected DLL and launcher." ON)
option(SC13_BUILD_TESTS "Build unit tests." ON)
option(SC13_INSTALL_DEVTOOLS "Build and install reverse-engineering and optional AI tools under simoder/DevTools." OFF)
option(
    SC13_ENABLE_DISCOVERY_TRACE
    "Enable the retired package-I/O and executable-image discovery instrumentation."
    OFF
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

function(sc13_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(sc13_core STATIC
    src/config/loader_config.cpp
    src/core/patched_resource_cache.cpp
    src/core/resource_filter.cpp
    src/core/runtime_property_table.cpp
    src/core/sha256.cpp
    src/core/tgi.cpp
    src/formats/dbpf/dbpf_reader.cpp
    src/formats/dbpf/refpack.cpp
    src/formats/prop/prop_document.cpp
    src/formats/toon/toon_document.cpp
    src/logging/log_event.cpp
    src/memory/signature.cpp
    src/mods/mod_definition_parser.cpp
    src/mods/mod_manager.cpp
    src/mods/mod_state_store.cpp
    src/mods/mod_types.cpp
    src/mods/patch_registry.cpp
    src/reverse/pe_file.cpp
    src/runtime/runtime_resource_cache.cpp
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
    set_property(DIRECTORY "${minhook_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)

    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.92.9
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(imgui)
    add_library(sc13_imgui STATIC
        "${imgui_SOURCE_DIR}/imgui.cpp"
        "${imgui_SOURCE_DIR}/imgui_draw.cpp"
        "${imgui_SOURCE_DIR}/imgui_tables.cpp"
        "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
    )
    target_include_directories(
        sc13_imgui
        PUBLIC "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends"
    )
    target_compile_definitions(sc13_imgui PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    set_target_properties(sc13_imgui PROPERTIES FOLDER "Dependencies")

    add_library(sc13modloader SHARED
        src/bootstrap/exports.def
        src/bootstrap/dll_main.cpp
        src/bootstrap/loader.cpp
        src/hooks/file_trace.cpp
        src/hooks/game_log_capture.cpp
        src/hooks/resource_trace.cpp
        src/logging/async_logger.cpp
        src/logging/mod_logger.cpp
        src/reverse/game_build.cpp
        src/reverse/resource_symbols.cpp
        src/runtime/patch_signal.cpp
        src/ui/simoder_overlay.cpp
    )
    if(SC13_ENABLE_DISCOVERY_TRACE)
        target_sources(sc13modloader PRIVATE src/runtime/image_observer.cpp)
        target_compile_definitions(sc13modloader PRIVATE SC13_ENABLE_DISCOVERY_TRACE=1)
    endif()
    target_link_libraries(sc13modloader PRIVATE sc13_core sc13_imgui minhook version d3d9)
    target_include_directories(sc13modloader PRIVATE src)
    target_compile_definitions(sc13modloader PRIVATE SC13_RUNTIME_BUILD)
    set_target_properties(sc13modloader PROPERTIES OUTPUT_NAME "sc13modloader")
    sc13_enable_warnings(sc13modloader)

    add_executable(sc13-launcher src/bootstrap/launcher_main.cpp)
    target_link_libraries(sc13-launcher PRIVATE sc13_core)
    set_target_properties(sc13-launcher PROPERTIES OUTPUT_NAME "simoder")
    if(MSVC)
        target_link_options(
            sc13-launcher
            PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'"
        )
    endif()
    sc13_enable_warnings(sc13-launcher)

    install(
        TARGETS sc13modloader sc13-launcher
        RUNTIME DESTINATION simoder
        LIBRARY DESTINATION simoder
    )
    install(FILES Start-Simoder.bat DESTINATION .)
    install(PROGRAMS Start-Simoder.sh DESTINATION .)
    install(FILES packaging/Uninstall-Simoder.bat DESTINATION simoder)
    install(
        FILES "${minhook_SOURCE_DIR}/LICENSE.txt"
        DESTINATION simoder/licenses
        RENAME minhook-LICENSE.txt
    )
    install(
        FILES "${imgui_SOURCE_DIR}/LICENSE.txt"
        DESTINATION simoder/licenses
        RENAME imgui-LICENSE.txt
    )
    configure_file(
        cmake/InstallRuntimeData.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/InstallRuntimeData.cmake"
        @ONLY
    )
    install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/InstallRuntimeData.cmake")
    if(SC13_INSTALL_DEVTOOLS)
        find_program(SC13_DOTNET_EXECUTABLE dotnet REQUIRED)
        set(SC13_AI_DEVTOOLS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/devtools/ai")
        set(SC13_AI_DEVTOOLS_PUBLISH_ROOT "${CMAKE_CURRENT_BINARY_DIR}/ai-devtools")
        add_custom_target(sc13-ai-devtools ALL
            COMMAND
                "${SC13_DOTNET_EXECUTABLE}" build
                "${SC13_AI_DEVTOOLS_SOURCE_DIR}/Simoder.DevBridge.Tests/Simoder.DevBridge.Tests.csproj"
                --configuration "$<CONFIG>"
            COMMAND
                "${SC13_DOTNET_EXECUTABLE}" publish
                "${SC13_AI_DEVTOOLS_SOURCE_DIR}/Simoder.DevBridge/Simoder.DevBridge.csproj"
                --configuration "$<CONFIG>"
                --runtime win-x64
                --self-contained true
                --output "${SC13_AI_DEVTOOLS_PUBLISH_ROOT}/$<CONFIG>"
                -p:PublishSingleFile=false
                -p:DebugType=None
                -p:DebugSymbols=false
            COMMAND
                "${SC13_DOTNET_EXECUTABLE}" publish
                "${SC13_AI_DEVTOOLS_SOURCE_DIR}/Simoder.McpServer/Simoder.McpServer.csproj"
                --configuration "$<CONFIG>"
                --runtime win-x64
                --self-contained true
                --output "${SC13_AI_DEVTOOLS_PUBLISH_ROOT}/$<CONFIG>"
                -p:PublishSingleFile=false
                -p:DebugType=None
                -p:DebugSymbols=false
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMENT "Building optional self-contained Simoder AI DevTools"
            VERBATIM
        )
        install(
            TARGETS sc13-inspect sc13-pe-probe sc13-memory-probe
            RUNTIME DESTINATION simoder/DevTools
        )
        configure_file(
            cmake/InstallAiDevTools.cmake.in
            "${CMAKE_CURRENT_BINARY_DIR}/InstallAiDevTools.cmake"
            @ONLY
        )
        install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/InstallAiDevTools.cmake")
        install(
            FILES
                devtools/ai/scripts/Register-Simoder-MCP.ps1
                devtools/ai/scripts/Unregister-Simoder-MCP.ps1
                devtools/ai/README.md
            DESTINATION simoder/DevTools/AI
        )
    endif()
endif()

if(SC13_BUILD_TESTS)
    enable_testing()
    add_executable(sc13-tests tests/test_main.cpp)
    target_link_libraries(sc13-tests PRIVATE sc13_core)
    sc13_enable_warnings(sc13-tests)
    add_test(NAME sc13-tests COMMAND sc13-tests)
    if(SC13_INSTALL_DEVTOOLS)
        add_test(
            NAME simoder-devbridge-tests
            COMMAND
                "${SC13_DOTNET_EXECUTABLE}" run
                --project "${CMAKE_CURRENT_SOURCE_DIR}/devtools/ai/Simoder.DevBridge.Tests/Simoder.DevBridge.Tests.csproj"
                --configuration "$<CONFIG>"
                --no-build
        )
        set_tests_properties(simoder-devbridge-tests PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            TIMEOUT 45
        )
    endif()
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
