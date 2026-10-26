cmake_minimum_required(VERSION 3.16)
project(rss-external LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Este projeto deve ser compilado em x64. Use: cmake -B build -A x64  (MSVC)  ou  cmake -B build -G \"MinGW Makefiles\"  (w64devkit)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Release" "Debug" "RelWithDebInfo")
endif()

set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/imgui-master/imgui-master")

set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)

set(PROJECT_SOURCES
    src/main.cpp
    src/overlay.cpp
    src/renderer.cpp
    src/memory.cpp
    src/rbx.cpp
    src/autodive.cpp
)

add_executable(${PROJECT_NAME} WIN32
    ${PROJECT_SOURCES}
    ${IMGUI_SOURCES}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    include/
    ${CMAKE_SOURCE_DIR}
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    $<$<CONFIG:Release>:NDEBUG>
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W3
        /MP
        /EHsc
        /utf-8
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:Release>:/Oi>
        $<$<CONFIG:Release>:/GL>
        $<$<CONFIG:Release>:/GS->
        $<$<CONFIG:Release>:/Gy>
        $<$<CONFIG:Debug>:/Od>
        $<$<CONFIG:Debug>:/Zi>
    )

    target_link_options(${PROJECT_NAME} PRIVATE
        /SUBSYSTEM:WINDOWS
        /MACHINE:X64
        /MANIFEST:EMBED
        /MANIFESTUAC:NO
        /MANIFESTINPUT:${CMAKE_SOURCE_DIR}/rss-external.manifest
        $<$<CONFIG:Release>:/LTCG>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
        $<$<CONFIG:Debug>:/DEBUG:FULL>
    )

    target_link_libraries(${PROJECT_NAME} PRIVATE
        d3d11
        dxgi
        d3dcompiler
        dwmapi
        winhttp
    )
else()
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall
        -Wextra
        -Wno-unused-parameter
        -Wno-missing-field-initializers
        -fno-strict-aliasing
        $<$<CONFIG:Release>:-O2>
        $<$<CONFIG:Release>:-ffunction-sections>
        $<$<CONFIG:Release>:-fdata-sections>
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Debug>:-g>
    )

    target_link_options(${PROJECT_NAME} PRIVATE
        -mwindows
        $<$<CONFIG:Release>:-Wl,--gc-sections>
        $<$<CONFIG:Release>:-s>
    )

    target_link_libraries(${PROJECT_NAME} PRIVATE
        d3d11
        dxgi
        d3dcompiler
        dwmapi
        user32
        gdi32
        ole32
        oleaut32
        shell32
        advapi32
        uuid
        winhttp
    )
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/Debug"
)
