cmake_minimum_required(VERSION 3.16)
project(rss-external LANGUAGES CXX)

# -------------------------------------------------------------------
# Força x64 — rejeita configuração 32 bits
# -------------------------------------------------------------------
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Este projeto deve ser compilado em x64. Use: cmake -B build -A x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -------------------------------------------------------------------
# Força Release como padrão se nada for especificado
# -------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Release" "Debug" "RelWithDebInfo")
endif()

# -------------------------------------------------------------------
# Paths
# -------------------------------------------------------------------
set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/imgui-master/imgui-master")

# -------------------------------------------------------------------
# ImGui sources
# -------------------------------------------------------------------
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)

# -------------------------------------------------------------------
# Project sources
# -------------------------------------------------------------------
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

# -------------------------------------------------------------------
# Include directories
# -------------------------------------------------------------------
target_include_directories(${PROJECT_NAME} PRIVATE
    include/
    ${CMAKE_SOURCE_DIR}          # para offsets.h na raiz
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

# -------------------------------------------------------------------
# Link libraries (DirectX 11 + Win32)
# -------------------------------------------------------------------
target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d11
    dxgi
    d3dcompiler
    dwmapi
)

# -------------------------------------------------------------------
# Compiler flags — MSVC x64 Release
# -------------------------------------------------------------------
target_compile_options(${PROJECT_NAME} PRIVATE
    /W3          # warnings padrão
    /MP          # compilação paralela
    /EHsc        # C++ exceptions
    $<$<CONFIG:Release>:/O2>          # otimização máxima
    $<$<CONFIG:Release>:/Oi>          # intrinsics
    $<$<CONFIG:Release>:/GL>          # whole program optimization
    $<$<CONFIG:Release>:/GS->         # desativa stack security cookie (menor overhead)
    $<$<CONFIG:Release>:/Gy>          # function-level linking
    $<$<CONFIG:Debug>:/Od>            # sem otimização no Debug
    $<$<CONFIG:Debug>:/Zi>            # debug info completo
)

# -------------------------------------------------------------------
# Linker flags — x64 Release
# -------------------------------------------------------------------
target_link_options(${PROJECT_NAME} PRIVATE
    /SUBSYSTEM:WINDOWS              # sem console window
    /MACHINE:X64                    # garante x64
    /MANIFEST:EMBED                 # embute o manifesto no .exe
    /MANIFESTUAC:NO                 # desativa UAC padrão do MSBuild (evita conflito de level)
    /MANIFESTINPUT:${CMAKE_SOURCE_DIR}/rss-external.manifest  # UAC requireAdministrator
    $<$<CONFIG:Release>:/LTCG>      # link-time code generation (combina com /GL)
    $<$<CONFIG:Release>:/OPT:REF>   # remove funções não referenciadas
    $<$<CONFIG:Release>:/OPT:ICF>   # mescla funções idênticas
    $<$<CONFIG:Debug>:/DEBUG:FULL>  # PDB completo no Debug
)

# -------------------------------------------------------------------
# Define NDEBUG em Release (desativa asserts)
# -------------------------------------------------------------------
target_compile_definitions(${PROJECT_NAME} PRIVATE
    $<$<CONFIG:Release>:NDEBUG>
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

# -------------------------------------------------------------------
# Output: coloca o .exe direto em build/Release/ ou build/Debug/
# -------------------------------------------------------------------
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/Debug"
)
