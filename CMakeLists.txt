cmake_minimum_required(VERSION 3.16)
project(BlueDroidEmulator LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Application sources. The recursive glob includes the shared platform, HTTP
# and SDL compatibility sources (src/core/platform.cpp,
# src/core/http_client.cpp, src/ui/sdl_compat.cpp) on every platform.
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
)

if(WIN32)
    list(APPEND IMGUI_SOURCES
        ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
        ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
    )
else()
    list(APPEND IMGUI_SOURCES
        ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    )
    # The vendored imgui_impl_opengl3_loader.h provides the GL loader; do not
    # define IMGUI_IMPL_OPENGL_LOADER_CUSTOM here.
endif()

if(WIN32)
    add_executable(BlueDroidEmulator WIN32 ${SOURCES} ${IMGUI_SOURCES})
else()
    add_executable(BlueDroidEmulator ${SOURCES} ${IMGUI_SOURCES})
endif()

target_include_directories(BlueDroidEmulator PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/resources
)

if(WIN32)
    # Windows libs (unchanged).
    target_link_libraries(BlueDroidEmulator PRIVATE
        gdi32 user32 kernel32 shell32 comctl32 comdlg32 psapi
        d3d11 dxgi d3dcompiler dwmapi ole32 winhttp
    )
else()
    find_package(Threads REQUIRED)
    find_package(SDL2 QUIET)
    find_package(OpenGL QUIET)
    find_package(CURL QUIET)

    # SDL2: prefer imported targets, fall back to classic variables.
    if(TARGET SDL2::SDL2)
        target_link_libraries(BlueDroidEmulator PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(BlueDroidEmulator PRIVATE SDL2::SDL2-static)
    else()
        if(NOT SDL2_INCLUDE_DIRS AND SDL2_INCLUDE_DIR)
            set(SDL2_INCLUDE_DIRS ${SDL2_INCLUDE_DIR})
        endif()
        if(NOT SDL2_LIBRARIES AND SDL2_LIBRARY)
            set(SDL2_LIBRARIES ${SDL2_LIBRARY})
        endif()
        if(NOT SDL2_LIBRARIES)
            find_path(SDL2_INCLUDE_DIR NAMES SDL.h PATH_SUFFIXES SDL2)
            find_library(SDL2_LIBRARY NAMES SDL2)
            if(NOT SDL2_INCLUDE_DIR OR NOT SDL2_LIBRARY)
                message(FATAL_ERROR
                    "SDL2 development files not found (install libsdl2-dev / SDL2-devel / brew install sdl2)")
            endif()
            set(SDL2_INCLUDE_DIRS ${SDL2_INCLUDE_DIR})
            set(SDL2_LIBRARIES ${SDL2_LIBRARY})
        endif()
        target_include_directories(BlueDroidEmulator PRIVATE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(BlueDroidEmulator PRIVATE ${SDL2_LIBRARIES})
    endif()

    # OpenGL: imported target, platform framework, or plain GL library.
    if(TARGET OpenGL::GL)
        target_link_libraries(BlueDroidEmulator PRIVATE OpenGL::GL)
    elseif(APPLE)
        find_library(OPENGL_FRAMEWORK OpenGL)
        if(OPENGL_FRAMEWORK)
            target_link_libraries(BlueDroidEmulator PRIVATE ${OPENGL_FRAMEWORK})
        else()
            target_link_libraries(BlueDroidEmulator PRIVATE "-framework OpenGL")
        endif()
    else()
        target_link_libraries(BlueDroidEmulator PRIVATE GL)
    endif()

    # libcurl: imported target, classic variables, or raw library.
    if(TARGET CURL::libcurl)
        target_link_libraries(BlueDroidEmulator PRIVATE CURL::libcurl)
    elseif(CURL_LIBRARIES)
        target_include_directories(BlueDroidEmulator PRIVATE ${CURL_INCLUDE_DIRS})
        target_link_libraries(BlueDroidEmulator PRIVATE ${CURL_LIBRARIES})
    else()
        find_path(CURL_INCLUDE_DIR NAMES curl/curl.h)
        find_library(CURL_LIBRARY NAMES curl libcurl)
        if(NOT CURL_INCLUDE_DIR OR NOT CURL_LIBRARY)
            message(FATAL_ERROR
                "libcurl development files not found (install libcurl4-openssl-dev / libcurl-devel / brew install curl)")
        endif()
        target_include_directories(BlueDroidEmulator PRIVATE ${CURL_INCLUDE_DIR})
        target_link_libraries(BlueDroidEmulator PRIVATE ${CURL_LIBRARY})
    endif()

    target_link_libraries(BlueDroidEmulator PRIVATE Threads::Threads)

    if(UNIX AND NOT APPLE)
        # dlopen() for the vendored ImGui GL loader; pthread is already linked
        # through Threads::Threads, but keep the explicit name for older distros.
        target_link_libraries(BlueDroidEmulator PRIVATE ${CMAKE_DL_LIBS} pthread)
    endif()

    if(APPLE)
        # SDL2 ships as Objective-C frameworks on macOS.
        foreach(fw Cocoa QuartzCore CoreFoundation)
            find_library(FW_${fw} ${fw})
            if(FW_${fw})
                target_link_libraries(BlueDroidEmulator PRIVATE ${FW_${fw}})
            else()
                target_link_libraries(BlueDroidEmulator PRIVATE "-framework ${fw}")
            endif()
        endforeach()
        target_link_options(BlueDroidEmulator PRIVATE "-ObjC")
    endif()
endif()

# Output
set_target_properties(BlueDroidEmulator PROPERTIES
    OUTPUT_NAME BlueDroidEmulator
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
