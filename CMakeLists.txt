cmake_minimum_required(VERSION 3.16)
project(BlueDroidEmulator LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Application sources
file(GLOB_RECURSE SOURCES
    src/*.cpp
    src/*.c
)

# Dear ImGui + the provided dark/purple UI helper.
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/resources/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)

add_executable(BlueDroidEmulator WIN32 ${SOURCES} ${IMGUI_SOURCES})

target_include_directories(BlueDroidEmulator PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/resources
)

# Windows libs
if(WIN32)
    target_link_libraries(BlueDroidEmulator PRIVATE
        gdi32 user32 kernel32 shell32 comctl32 comdlg32 psapi
        d3d11 dxgi d3dcompiler dwmapi ole32 winhttp
    )
endif()

# Output
set_target_properties(BlueDroidEmulator PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
