#  SPDX-License-Identifier: MIT
#
#  ES-DE Frontend
#  CMakeLists.txt (uwp)
#
#  CMake configuration for the UWP (Universal Windows Platform) build, primarily intended
#  for Xbox consoles in Developer Mode.
#
#  This is a separate project from the main CMakeLists.txt as the UWP build uses vcpkg for
#  all dependencies, the Direct3D 12 renderer instead of OpenGL and an AppX package
#  instead of an installer. See uwp/README.md for build instructions.
#
#  Configure (from the repository root, in a "Developer PowerShell for VS 2022"):
#    cmake -S uwp -B build-uwp -G "Visual Studio 17 2022" -A x64 `
#          "-DCMAKE_SYSTEM_NAME=WindowsStore" "-DCMAKE_SYSTEM_VERSION=10.0" `
#          -DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake `
#          -DVCPKG_TARGET_TRIPLET=x64-uwp
#

cmake_minimum_required(VERSION 3.21)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
    message(FATAL_ERROR "The UWP build requires -DCMAKE_SYSTEM_NAME=WindowsStore "
                        "-DCMAKE_SYSTEM_VERSION=10.0, see uwp/README.md")
endif()

project(ES-DE-UWP LANGUAGES C CXX)

set(ES_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ES_UWP_PDF "Enable PDF manuals using Poppler (GPL, requires the vcpkg pdf feature)" OFF)
set(UWP_PACKAGE_NAME "ESDE.UWP" CACHE STRING "Package identity name")
set(UWP_PUBLISHER "CN=ES-DE UWP" CACHE STRING "Package publisher, must match the certificate")
set(UWP_PUBLISHER_DISPLAY_NAME "ES-DE UWP" CACHE STRING "Publisher display name")
set(UWP_CERTIFICATE "" CACHE FILEPATH "Optional .pfx certificate used to sign the package")

#---------------------------------------------------------------------------------------------------
# Version, read from ApplicationVersion.h.

file(READ ${ES_ROOT}/es-core/src/ApplicationVersion.h VERSION_HEADER)
string(REGEX MATCH "PROGRAM_VERSION_MAJOR +([0-9]+)" _ ${VERSION_HEADER})
set(ES_VERSION_MAJOR ${CMAKE_MATCH_1})
string(REGEX MATCH "PROGRAM_VERSION_MINOR +([0-9]+)" _ ${VERSION_HEADER})
set(ES_VERSION_MINOR ${CMAKE_MATCH_1})
string(REGEX MATCH "PROGRAM_VERSION_MAINTENANCE +([0-9]+)" _ ${VERSION_HEADER})
set(ES_VERSION_MAINTENANCE ${CMAKE_MATCH_1})
if(NOT ES_VERSION_MAJOR)
    set(ES_VERSION_MAJOR 3)
    set(ES_VERSION_MINOR 4)
    set(ES_VERSION_MAINTENANCE 1)
endif()
set(UWP_PACKAGE_VERSION "${ES_VERSION_MAJOR}.${ES_VERSION_MINOR}.${ES_VERSION_MAINTENANCE}.0")
message(STATUS "ES-DE UWP package version ${UWP_PACKAGE_VERSION}")

#---------------------------------------------------------------------------------------------------
# Dependencies (vcpkg, see uwp/vcpkg.json).

find_package(CURL CONFIG REQUIRED)
find_package(Freetype REQUIRED)
find_package(ICU REQUIRED COMPONENTS uc i18n data)
find_package(pugixml CONFIG REQUIRED)
find_package(SDL2 CONFIG REQUIRED)
find_package(FFMPEG REQUIRED)
find_package(freeimage CONFIG REQUIRED)
find_package(Intl REQUIRED)
find_package(ZLIB REQUIRED)

# libgit2 is optional, it's only used by the theme downloader. It has used different CMake
# package names in vcpkg over time.
find_package(unofficial-git2 CONFIG QUIET)
find_package(libgit2 CONFIG QUIET)
if(TARGET unofficial::git2::git2)
    set(GIT2_TARGET unofficial::git2::git2)
elseif(TARGET libgit2::libgit2package)
    set(GIT2_TARGET libgit2::libgit2package)
elseif(TARGET libgit2::libgit2)
    set(GIT2_TARGET libgit2::libgit2)
else()
    find_library(GIT2_TARGET NAMES git2)
endif()
if(GIT2_TARGET)
    message(STATUS "libgit2 found, the theme downloader is enabled")
else()
    message(WARNING "libgit2 not found, building without the theme downloader")
    set(GIT2_TARGET "")
endif()

set(PDF_LIBRARIES "")
if(ES_UWP_PDF)
    find_package(unofficial-poppler CONFIG QUIET)
    find_path(POPPLER_CPP_INCLUDE_DIR poppler-document.h PATH_SUFFIXES poppler/cpp REQUIRED)
    if(TARGET unofficial::poppler::poppler-cpp)
        set(PDF_LIBRARIES unofficial::poppler::poppler-cpp)
    else()
        find_library(POPPLER_CPP_LIBRARY NAMES poppler-cpp REQUIRED)
        find_library(POPPLER_LIBRARY NAMES poppler REQUIRED)
        set(PDF_LIBRARIES ${POPPLER_CPP_LIBRARY} ${POPPLER_LIBRARY})
    endif()
endif()

#---------------------------------------------------------------------------------------------------
# Visual Studio defaults for UWP projects that don't fit this project. They're overridden for
# all generated projects (ES-DE and the bundled libraries) with a Directory.Build.targets
# file, which MSBuild imports for every project in the build directory:
# - C++/CX (/ZW) is off: ES-DE uses C++/WinRT, which is standard C++, and C++/CX reserves
#   names like "generic" that FreeType uses (FT_Face::generic).
# - SDL checks (/sdl) are off: they turn deprecation warnings (C4996) into errors.

file(WRITE ${CMAKE_BINARY_DIR}/Directory.Build.targets [=[
<Project>
  <ItemDefinitionGroup>
    <ClCompile>
      <CompileAsWinRT>false</CompileAsWinRT>
      <SDLCheck>false</SDLCheck>
      <PreprocessorDefinitions>_CRT_SECURE_NO_WARNINGS;%(PreprocessorDefinitions)</PreprocessorDefinitions>
    </ClCompile>
  </ItemDefinitionGroup>
</Project>
]=])

#---------------------------------------------------------------------------------------------------
# HarfBuzz. The vcpkg port doesn't support UWP (its Meson build fails), so the single-file
# amalgamation is built here instead. HarfBuzz itself supports UWP (CreateFile2 and
# MapViewOfFileFromApp in hb-blob.cc). The hash is the one used by the vcpkg port.

include(FetchContent)
set(HARFBUZZ_VERSION 14.2.1)
FetchContent_Declare(harfbuzz
    URL https://github.com/harfbuzz/harfbuzz/archive/${HARFBUZZ_VERSION}.tar.gz
    URL_HASH SHA512=b7642a81eb021bf96cf8c91c5ebdde7f4fdfd40c76db722f00cf001125f4b81b954d08485774d2b23318d49b1e954fa0189ba8f10db56d148f33f9d90891d0cb
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    # Only download, HarfBuzz's own build files are not used.
    SOURCE_SUBDIR do-not-add)
FetchContent_MakeAvailable(harfbuzz)

# The vcpkg FreeType port renames FT_FaceRec::generic to GenericFromFreeTypeLibrary for UWP
# (0003-Fix-UWP.patch), defining and undefining a "generic" macro around the struct. So the
# macro is defined again after FreeType has been included, for HarfBuzz's hb-ft.cc.
file(WRITE ${CMAKE_BINARY_DIR}/harfbuzz-uwp.cc [=[
#include <ft2build.h>
#include FT_FREETYPE_H
#define generic GenericFromFreeTypeLibrary
#include "harfbuzz.cc"
]=])
add_library(harfbuzz STATIC ${CMAKE_BINARY_DIR}/harfbuzz-uwp.cc)
target_include_directories(harfbuzz PUBLIC ${harfbuzz_SOURCE_DIR}/src)
target_compile_definitions(harfbuzz PRIVATE HAVE_FREETYPE)
target_compile_options(harfbuzz PRIVATE /bigobj /utf-8 /wd4244 /wd4267)
target_link_libraries(harfbuzz PUBLIC Freetype::Freetype)
add_library(harfbuzz::harfbuzz ALIAS harfbuzz)

#---------------------------------------------------------------------------------------------------
# Bundled libraries (built as static libraries to avoid having to deploy additional DLLs).

set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(BUILD_SHARED_LIBS OFF)
set(LOTTIE_THREAD OFF CACHE BOOL "" FORCE)
set(LOTTIE_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory(${ES_ROOT}/external/lunasvg ${CMAKE_BINARY_DIR}/external/lunasvg EXCLUDE_FROM_ALL)
add_subdirectory(${ES_ROOT}/external/rlottie ${CMAKE_BINARY_DIR}/external/rlottie EXCLUDE_FROM_ALL)
target_compile_options(lunasvg PRIVATE /wd4251 /wd4267)
target_compile_options(rlottie PRIVATE /wd4244 /wd4251 /wd4263 /wd4334 /wd4267 /wd4530 /wd4996)

#---------------------------------------------------------------------------------------------------
# Sources.

file(GLOB_RECURSE CORE_SOURCES ${ES_ROOT}/es-core/src/*.cpp)
file(GLOB_RECURSE CORE_HEADERS ${ES_ROOT}/es-core/src/*.h)
file(GLOB_RECURSE APP_SOURCES ${ES_ROOT}/es-app/src/*.cpp)
file(GLOB_RECURSE APP_HEADERS ${ES_ROOT}/es-app/src/*.h)

# OpenGL renderer and other platforms.
list(FILTER CORE_SOURCES EXCLUDE REGEX "(RendererOpenGL|ShaderOpenGL|PlatformUtilAndroid)\\.cpp$")
list(FILTER CORE_HEADERS EXCLUDE REGEX "(RendererOpenGL|ShaderOpenGL|PlatformUtilAndroid|BluetoothStatusApple)\\.h$")
list(FILTER APP_SOURCES EXCLUDE REGEX "Android")

if(NOT GIT2_TARGET)
    list(FILTER APP_SOURCES EXCLUDE REGEX "GuiThemeDownloader\\.cpp$")
endif()

set(UWP_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/UWPMain.cpp)
if(ES_UWP_PDF)
    list(APPEND UWP_SOURCES ${ES_ROOT}/es-pdf-converter/src/ConvertPDF.cpp)
else()
    list(APPEND UWP_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/ConvertPDFStub.cpp)
endif()

#---------------------------------------------------------------------------------------------------
# Localization message catalogs (optional, requires msgfmt from gettext).

find_program(MSGFMT_BINARY msgfmt
             HINTS ${VCPKG_INSTALLED_DIR}/${VCPKG_HOST_TRIPLET}/tools/gettext/bin)
if(MSGFMT_BINARY)
    file(GLOB PO_FILES ${ES_ROOT}/locale/po/*.po)
    foreach(PO_FILE ${PO_FILES})
        get_filename_component(PO_BASENAME ${PO_FILE} NAME_WE)
        file(MAKE_DIRECTORY ${ES_ROOT}/resources/locale/${PO_BASENAME}/LC_MESSAGES)
        execute_process(COMMAND ${MSGFMT_BINARY} -c -o
                        ${ES_ROOT}/resources/locale/${PO_BASENAME}/LC_MESSAGES/${PO_BASENAME}.mo
                        ${PO_FILE})
    endforeach()
else()
    message(WARNING "msgfmt not found, translations will only be included if the .mo files "
                    "already exist in resources/locale")
endif()

#---------------------------------------------------------------------------------------------------
# Package content: manifest, logos, resources, themes and the dependency DLLs.

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/Package.appxmanifest.in
               ${CMAKE_CURRENT_BINARY_DIR}/Package.appxmanifest @ONLY)
set(UWP_MANIFEST ${CMAKE_CURRENT_BINARY_DIR}/Package.appxmanifest)
set_source_files_properties(${UWP_MANIFEST} PROPERTIES VS_DEPLOYMENT_CONTENT 1)

set(PACKAGE_CONTENT "")

# Adds files to the package, keeping their path relative to baseDir.
function(add_package_content baseDir targetDir)
    file(GLOB_RECURSE files ${baseDir}/*)
    foreach(file ${files})
        file(RELATIVE_PATH relative ${baseDir} ${file})
        get_filename_component(relativeDir ${relative} DIRECTORY)
        if(relativeDir STREQUAL "")
            set(location ${targetDir})
        else()
            set(location ${targetDir}/${relativeDir})
        endif()
        set_source_files_properties(${file} PROPERTIES
                                    VS_DEPLOYMENT_CONTENT 1
                                    VS_DEPLOYMENT_LOCATION ${location}
                                    HEADER_FILE_ONLY TRUE)
        list(APPEND PACKAGE_CONTENT ${file})
    endforeach()
    set(PACKAGE_CONTENT ${PACKAGE_CONTENT} PARENT_SCOPE)
endfunction()

add_package_content(${CMAKE_CURRENT_SOURCE_DIR}/Assets Assets)
add_package_content(${ES_ROOT}/resources resources)
add_package_content(${ES_ROOT}/themes themes)

# The dependencies are built as DLLs by the x64-uwp triplet, so they need to be included in
# the package next to ES-DE.exe.
set(VCPKG_BIN_DIR ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin)
set(VCPKG_DEBUG_BIN_DIR ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin)
file(GLOB RELEASE_DLLS ${VCPKG_BIN_DIR}/*.dll)
file(GLOB DEBUG_DLLS ${VCPKG_DEBUG_BIN_DIR}/*.dll)
foreach(dll ${RELEASE_DLLS})
    set_source_files_properties(${dll} PROPERTIES VS_DEPLOYMENT_CONTENT $<NOT:$<CONFIG:Debug>>
                                                  HEADER_FILE_ONLY TRUE)
endforeach()
foreach(dll ${DEBUG_DLLS})
    set_source_files_properties(${dll} PROPERTIES VS_DEPLOYMENT_CONTENT $<CONFIG:Debug>
                                                  HEADER_FILE_ONLY TRUE)
endforeach()

source_group("Package\\Assets" REGULAR_EXPRESSION "/uwp/Assets/")
source_group("Package\\Resources" REGULAR_EXPRESSION "/resources/")
source_group("Package\\Themes" REGULAR_EXPRESSION "/themes/")
source_group("Package\\DLLs" REGULAR_EXPRESSION "\\.dll$")

#---------------------------------------------------------------------------------------------------
# Application target.

add_executable(ES-DE WIN32
               ${CORE_SOURCES} ${CORE_HEADERS}
               ${APP_SOURCES} ${APP_HEADERS}
               ${UWP_SOURCES}
               ${UWP_MANIFEST}
               ${PACKAGE_CONTENT}
               ${RELEASE_DLLS} ${DEBUG_DLLS})

target_compile_definitions(ES-DE PRIVATE
                           ES_UWP
                           USE_D3D12_RENDERER
                           USE_BUNDLED_CERTIFICATES
                           ROMM_ZIP_SUPPORT
                           UNICODE _UNICODE
                           NOMINMAX
                           WIN32_LEAN_AND_MEAN
                           _CRT_SECURE_NO_WARNINGS
                           _WIN32_WINNT=0x0A00
                           WINVER=0x0A00
                           GLM_FORCE_CXX17
                           GLM_FORCE_XYZW_ONLY
                           $<$<CONFIG:Debug>:_DEBUG>
                           $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
if(ES_UWP_PDF)
    target_compile_definitions(ES-DE PRIVATE ES_UWP_PDF)
endif()
if(NOT GIT2_TARGET)
    target_compile_definitions(ES-DE PRIVATE ES_NO_THEME_DOWNLOADER)
endif()

target_include_directories(ES-DE PRIVATE
                           ${ES_ROOT}/es-core/src
                           ${ES_ROOT}/es-app/src
                           ${ES_ROOT}/es-pdf-converter/src
                           ${ES_ROOT}/external/CImg
                           ${ES_ROOT}/external/glm
                           ${ES_ROOT}/external/lunasvg/include
                           ${ES_ROOT}/external/rapidjson/include
                           ${ES_ROOT}/external/rlottie/inc
                           ${ES_ROOT}/external/utfcpp/source
                           ${FFMPEG_INCLUDE_DIRS}
                           ${POPPLER_CPP_INCLUDE_DIR})

target_compile_options(ES-DE PRIVATE /EHsc /utf-8 /bigobj /Zc:__cplusplus /MP
                       /wd4244 /wd4267 /wd4996)

target_link_libraries(ES-DE PRIVATE
                      lunasvg
                      rlottie
                      CURL::libcurl
                      Freetype::Freetype
                      harfbuzz::harfbuzz
                      ICU::uc ICU::i18n ICU::data
                      pugixml::pugixml
                      SDL2::SDL2
                      ${FFMPEG_LIBRARIES}
                      freeimage::FreeImage
                      Intl::Intl
                      ZLIB::ZLIB
                      ${GIT2_TARGET}
                      ${PDF_LIBRARIES}
                      d3d12 dxgi d3dcompiler
                      windowsapp)

target_link_options(ES-DE PRIVATE /APPCONTAINER /WINMD:NO)

set_target_properties(ES-DE PROPERTIES
                      OUTPUT_NAME "ES-DE"
                      VS_WINRT_COMPONENT OFF
                      VS_WINRT_EXTENSIONS OFF
                      VS_GLOBAL_CharacterSet "Unicode"
                      VS_GLOBAL_DefaultLanguage "en-US"
                      VS_GLOBAL_AppContainerApplication "true"
                      VS_GLOBAL_ApplicationType "Windows Store"
                      VS_GLOBAL_ApplicationTypeRevision "10.0"
                      VS_GLOBAL_WindowsTargetPlatformMinVersion "10.0.19041.0"
                      VS_GLOBAL_AppxBundle "Never"
                      VS_GLOBAL_AppxPackageSigningEnabled "true"
                      VS_STARTUP_PROJECT ES-DE)

if(UWP_CERTIFICATE)
    set_target_properties(ES-DE PROPERTIES
                          VS_GLOBAL_PackageCertificateKeyFile ${UWP_CERTIFICATE})
endif()

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ES-DE)
