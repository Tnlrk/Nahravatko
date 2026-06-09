cmake_minimum_required(VERSION 3.20)
project(Nahravatko VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

qt_add_executable(Nahravatko WIN32
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow.h
    src/recorder_engine.cpp
    src/recorder_engine.h
    src/video_capture.cpp
    src/video_capture.h
    src/audio_capture.cpp
    src/audio_capture.h
    src/win32_utils.cpp
    src/win32_utils.h
    app.rc
)

target_compile_definitions(Nahravatko PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
)

# /utf-8: zdrojáky jsou v UTF-8 (české řetězce); /bigobj kvůli WinRT hlavičkám
target_compile_options(Nahravatko PRIVATE /utf-8 /permissive- /bigobj)

# C++/WinRT projekční hlavičky (winrt/Windows.Graphics.Capture.h apod.) nejsou
# v default INCLUDE z vcvars — přidáme je z Windows SDK.
set(_cppwinrt_inc "")
if(DEFINED ENV{WindowsSdkDir} AND DEFINED ENV{WindowsSDKVersion})
    string(REGEX REPLACE "[\\\\/]$" "" _sdkver "$ENV{WindowsSDKVersion}")
    set(_cppwinrt_inc "$ENV{WindowsSdkDir}Include/${_sdkver}/cppwinrt")
endif()
if(NOT EXISTS "${_cppwinrt_inc}")
    set(_cppwinrt_inc "C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/cppwinrt")
endif()
if(EXISTS "${_cppwinrt_inc}")
    target_include_directories(Nahravatko PRIVATE "${_cppwinrt_inc}")
    message(STATUS "C++/WinRT include: ${_cppwinrt_inc}")
else()
    message(WARNING "C++/WinRT hlavičky nenalezeny — WGC se nepřeloží")
endif()

# Vložit pouze náš manifest (potlačit linkerem generovaný, aby nevznikl konflikt)
target_link_options(Nahravatko PRIVATE /MANIFEST:NO)

target_link_libraries(Nahravatko PRIVATE
    Qt6::Widgets
    d3d11
    dxgi
    ole32
    oleaut32
    propsys
    uuid
    windowsapp   # WinRT umbrella (WGC)
    dwmapi       # DWM cloaked-window filtr při výčtu oken
    version      # FileDescription (název aplikace okna)
)

# --- Post-build: přibalit ffmpeg.exe vedle výsledného .exe ---
set(FFMPEG_DIR "${CMAKE_SOURCE_DIR}/tools/ffmpeg/ffmpeg-8.1.1-essentials_build")
set(FFMPEG_EXE "${FFMPEG_DIR}/bin/ffmpeg.exe")
if(EXISTS "${FFMPEG_EXE}")
    add_custom_command(TARGET Nahravatko POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFMPEG_EXE}" "$<TARGET_FILE_DIR:Nahravatko>/ffmpeg.exe"
        COMMENT "Kopiruji ffmpeg.exe do vystupni slozky")
else()
    message(WARNING "ffmpeg.exe nenalezen na ${FFMPEG_EXE} — nebude pribalen")
endif()

# --- Post-build: licence pro distribuci (naše MIT + GPL FFmpegu / compliance) ---
if(EXISTS "${CMAKE_SOURCE_DIR}/LICENSE")
    add_custom_command(TARGET Nahravatko POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/LICENSE" "$<TARGET_FILE_DIR:Nahravatko>/LICENSE.txt")
endif()
if(EXISTS "${FFMPEG_DIR}/LICENSE")
    add_custom_command(TARGET Nahravatko POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFMPEG_DIR}/LICENSE" "$<TARGET_FILE_DIR:Nahravatko>/FFmpeg-LICENSE.txt")
endif()
if(EXISTS "${FFMPEG_DIR}/README.txt")
    add_custom_command(TARGET Nahravatko POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFMPEG_DIR}/README.txt" "$<TARGET_FILE_DIR:Nahravatko>/FFmpeg-README.txt")
endif()

# --- Post-build: nasadit Qt runtime DLL přes windeployqt ---
get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
get_filename_component(_qt_bin "${_qmake}" DIRECTORY)
find_program(WINDEPLOYQT_EXE windeployqt HINTS "${_qt_bin}")
if(WINDEPLOYQT_EXE)
    add_custom_command(TARGET Nahravatko POST_BUILD
        COMMAND "${WINDEPLOYQT_EXE}" --no-translations --no-compiler-runtime
            "$<TARGET_FILE:Nahravatko>"
        COMMENT "Nasazuji Qt DLL (windeployqt)")
endif()

# --- Post-build: VC++ runtime DLL (app poběží na čistém Windows bez instalace redistu) ---
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP ON)
include(InstallRequiredSystemLibraries)
foreach(_rt ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS})
    add_custom_command(TARGET Nahravatko POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_rt}" "$<TARGET_FILE_DIR:Nahravatko>"
        COMMENT "Kopíruji VC++ runtime")
endforeach()
