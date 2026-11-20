cmake_minimum_required(VERSION 3.20)
project(NDI2SDI VERSION 1.1.0 LANGUAGES CXX RC)

if(NOT MSVC)
  message(FATAL_ERROR "NDI2SDI must be built with MSVC (Visual Studio 2022 / Build Tools).")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "NDI2SDI must be built for x64.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static CRT: the exe needs no Visual C++ redistributable on the church PC.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---- NDI SDK (headers only; the runtime DLL is loaded dynamically at run time) ----
set(NDI_SDK_DIR "$ENV{NDI_SDK_DIR}" CACHE PATH "NDI SDK root (contains Include/Processing.NDI.Lib.h)")
if(NOT EXISTS "${NDI_SDK_DIR}/Include/Processing.NDI.Lib.h")
  foreach(candidate "C:/Program Files/NDI/NDI 6 SDK" "C:/Program Files/NDI/NDI 5 SDK")
    if(EXISTS "${candidate}/Include/Processing.NDI.Lib.h")
      set(NDI_SDK_DIR "${candidate}" CACHE PATH "NDI SDK root" FORCE)
      break()
    endif()
  endforeach()
endif()
if(NOT EXISTS "${NDI_SDK_DIR}/Include/Processing.NDI.Lib.h")
  message(FATAL_ERROR "NDI SDK not found. Install the NDI 6 SDK or pass -DNDI_SDK_DIR=<path>.")
endif()
message(STATUS "NDI SDK: ${NDI_SDK_DIR}")

# ---- DeckLink API ----
# Headers are generated at compile time with #import from the type library that Blackmagic
# Desktop Video registers, so building requires Desktop Video to be installed on the build PC.

add_executable(NDI2SDI WIN32
  src/main.cpp
  src/Util.cpp            src/Util.h
  src/Log.cpp             src/Log.h
  src/Json.cpp            src/Json.h
  src/Config.cpp          src/Config.h
  src/Autostart.cpp       src/Autostart.h
  src/FrameConvert.cpp    src/FrameConvert.h
  src/NdiLib.cpp          src/NdiLib.h
  src/NdiReceiver.cpp     src/NdiReceiver.h
  src/VideoSource.h
  src/Displays.cpp        src/Displays.h
  src/DisplayCapture.cpp  src/DisplayCapture.h
  src/DeckLink.cpp        src/DeckLink.h
  src/Pipeline.cpp        src/Pipeline.h
  src/TrayApp.cpp         src/TrayApp.h
  src/SettingsDialog.cpp  src/SettingsDialog.h
  res/app.rc              res/resource.h
  res/app.manifest
)

target_include_directories(NDI2SDI SYSTEM PRIVATE "${NDI_SDK_DIR}/Include")
target_include_directories(NDI2SDI PRIVATE src res "${CMAKE_CURRENT_BINARY_DIR}")

target_compile_definitions(NDI2SDI PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)

target_compile_options(NDI2SDI PRIVATE /W4 /WX /permissive- /utf-8 /Zc:__cplusplus /EHsc /external:W0)

target_link_libraries(NDI2SDI PRIVATE comctl32 shell32 ole32 oleaut32 advapi32 user32 gdi32 d3d11 dxgi)

# Embed our manifest (common controls v6, per-monitor DPI) instead of the linker's default one.
target_link_options(NDI2SDI PRIVATE /MANIFEST:EMBED /MANIFESTUAC:NO)

set_target_properties(NDI2SDI PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ---- Unit tests for the pure C++ parts (frame conversion, JSON) ----
add_executable(NDI2SDI_tests tests/test_main.cpp src/FrameConvert.cpp src/Json.cpp)
target_include_directories(NDI2SDI_tests PRIVATE src)
target_compile_options(NDI2SDI_tests PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
set_target_properties(NDI2SDI_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
