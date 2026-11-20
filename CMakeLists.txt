cmake_minimum_required(VERSION 3.20)
project(Screen2SDI VERSION 1.2.0 LANGUAGES CXX RC)

if(NOT MSVC)
  message(FATAL_ERROR "Screen2SDI must be built with MSVC (Visual Studio 2022 / Build Tools).")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Screen2SDI must be built for x64.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static CRT: the exe needs no Visual C++ redistributable on the playout PC.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---- NDI SDK (OPTIONAL; headers only, the runtime DLL is loaded dynamically at run time) ----
# Without the SDK the app is built for display capture only (virtual/physical monitor -> SDI).
option(SCREEN2SDI_WITHOUT_NDI "Build without NDI support (display capture only)" OFF)
set(NDI_SDK_DIR "$ENV{NDI_SDK_DIR}" CACHE PATH "NDI SDK root (contains Include/Processing.NDI.Lib.h)")
set(SCREEN2SDI_HAS_NDI 0)
if(NOT SCREEN2SDI_WITHOUT_NDI)
  if(NOT EXISTS "${NDI_SDK_DIR}/Include/Processing.NDI.Lib.h")
    foreach(candidate "C:/Program Files/NDI/NDI 6 SDK" "C:/Program Files/NDI/NDI 5 SDK")
      if(EXISTS "${candidate}/Include/Processing.NDI.Lib.h")
        set(NDI_SDK_DIR "${candidate}" CACHE PATH "NDI SDK root" FORCE)
        break()
      endif()
    endforeach()
  endif()
  if(EXISTS "${NDI_SDK_DIR}/Include/Processing.NDI.Lib.h")
    set(SCREEN2SDI_HAS_NDI 1)
  else()
    message(WARNING "NDI SDK not found: building WITHOUT NDI (display capture only). "
                    "Install the NDI 6 SDK or pass -DNDI_SDK_DIR=<path> to include NDI sources.")
  endif()
endif()
if(SCREEN2SDI_HAS_NDI)
  message(STATUS "NDI: included (SDK ${NDI_SDK_DIR})")
  set(NDI_RECEIVER_SRC src/NdiReceiver.cpp)
else()
  message(STATUS "NDI: not included - display capture only")
  set(NDI_RECEIVER_SRC src/NdiReceiverStub.cpp)
endif()

# ---- DeckLink API ----
# Headers are generated at compile time with #import from the type library that Blackmagic
# Desktop Video registers, so building requires Desktop Video to be installed on the build PC.

add_executable(Screen2SDI WIN32
  src/main.cpp
  src/Util.cpp            src/Util.h
  src/Log.cpp             src/Log.h
  src/Json.cpp            src/Json.h
  src/Config.cpp          src/Config.h
  src/Autostart.cpp       src/Autostart.h
  src/FrameConvert.cpp    src/FrameConvert.h
  src/NdiLib.cpp          src/NdiLib.h
  ${NDI_RECEIVER_SRC}     src/NdiReceiver.h
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

if(SCREEN2SDI_HAS_NDI)
  target_include_directories(Screen2SDI SYSTEM PRIVATE "${NDI_SDK_DIR}/Include")
endif()
target_include_directories(Screen2SDI PRIVATE src res "${CMAKE_CURRENT_BINARY_DIR}")

target_compile_definitions(Screen2SDI PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00
  SCREEN2SDI_HAS_NDI=${SCREEN2SDI_HAS_NDI})

target_compile_options(Screen2SDI PRIVATE /W4 /WX /permissive- /utf-8 /Zc:__cplusplus /EHsc /external:W0)

target_link_libraries(Screen2SDI PRIVATE comctl32 shell32 ole32 oleaut32 advapi32 user32 gdi32 d3d11 dxgi)

# Embed our manifest (common controls v6, per-monitor DPI) instead of the linker's default one.
target_link_options(Screen2SDI PRIVATE /MANIFEST:EMBED /MANIFESTUAC:NO)

set_target_properties(Screen2SDI PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ---- Unit tests for the pure C++ parts (frame conversion, JSON) ----
add_executable(Screen2SDI_tests tests/test_main.cpp src/FrameConvert.cpp src/Json.cpp)
target_include_directories(Screen2SDI_tests PRIVATE src)
target_compile_options(Screen2SDI_tests PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
set_target_properties(Screen2SDI_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
