cmake_minimum_required(VERSION 3.16)
cmake_policy(SET CMP0091 NEW)  # honour CMAKE_MSVC_RUNTIME_LIBRARY
project(XboxToolkit CXX)

# Link the MSVC runtime statically so the .exe runs without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)
include(FetchContent)

option(BUILD_GUI "Build the ImGui desktop app" ON)
option(BUILD_UI_SMOKE_TEST "Build the headless ImGui smoke test of the builder" ON)

# JSON parser for mod files
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(json)

# ---- console client + mod loader library (no GUI dependencies) ----
add_library(xbox_core STATIC src/xbdm.cpp src/xrpc.cpp src/mod.cpp src/scanner.cpp)
target_include_directories(xbox_core PUBLIC src)
target_link_libraries(xbox_core PUBLIC Threads::Threads nlohmann_json::nlohmann_json)
if(WIN32)
    target_link_libraries(xbox_core PUBLIC ws2_32)
endif()

enable_testing()

# ---- tests against the in-process mock console (POSIX only) ----
if(NOT WIN32)
    add_executable(core_test tests/core_test.cpp)
    target_link_libraries(core_test PRIVATE xbox_core)
    add_test(NAME core_test COMMAND core_test)
endif()

# Scanner tests use fake memory, so they run everywhere (including Windows).
add_executable(scanner_test tests/scanner_test.cpp)
target_link_libraries(scanner_test PRIVATE xbox_core)
add_test(NAME scanner_test COMMAND scanner_test)

# ---- Dear ImGui core + the builder UI (no window system needed) ----
if(BUILD_GUI OR BUILD_UI_SMOKE_TEST)
    FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.9)
    FetchContent_MakeAvailable(imgui)

    add_library(imgui_core STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp)
    target_include_directories(imgui_core PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    # Keep ImGui's own consistency asserts on even in Release builds; the smoke test relies on them.
    if(MSVC)
        target_compile_options(imgui_core PRIVATE /UNDEBUG)
    else()
        target_compile_options(imgui_core PRIVATE -UNDEBUG)
    endif()

    add_library(builder_ui STATIC src/builder.cpp src/memtools.cpp)
    target_link_libraries(builder_ui PUBLIC xbox_core imgui_core)
endif()

if(BUILD_UI_SMOKE_TEST)
    add_executable(ui_smoke tests/ui_smoke.cpp)
    target_link_libraries(ui_smoke PRIVATE builder_ui)
    add_test(NAME ui_smoke COMMAND ui_smoke)
endif()

# ---- GUI (Dear ImGui + GLFW + OpenGL3), fetched at configure time ----
if(BUILD_GUI)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4)
    FetchContent_MakeAvailable(glfw)

    find_package(OpenGL REQUIRED)

    add_library(imgui_backends STATIC
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
    target_link_libraries(imgui_backends PUBLIC imgui_core glfw OpenGL::GL)

    add_executable(XboxToolkit src/main.cpp)
    target_link_libraries(XboxToolkit PRIVATE builder_ui imgui_backends)
    if(WIN32)
        # Build as a GUI app (no console window) with a normal main().
        set_target_properties(XboxToolkit PROPERTIES WIN32_EXECUTABLE ON)
        if(MSVC)
            target_link_options(XboxToolkit PRIVATE /ENTRY:mainCRTStartup)
        endif()
    endif()

    # Put the sample mod files next to the executable (the app looks for "mods" there).
    add_custom_command(TARGET XboxToolkit POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/mods $<TARGET_FILE_DIR:XboxToolkit>/mods)

    # `cmake --install <build> --prefix <dir>` produces a folder you can zip and share.
    install(TARGETS XboxToolkit RUNTIME DESTINATION .)
    install(DIRECTORY mods DESTINATION .)
    install(FILES README.md DESTINATION .)
endif()
