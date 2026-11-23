cmake_minimum_required(VERSION 3.20)
project(Propods LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "Propods Windows target requires Windows.")
endif()

if(NOT MSVC)
    message(FATAL_ERROR "Propods Windows target requires MSVC (cppwinrt).")
endif()

add_compile_definitions(
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
)

add_compile_options(
    /await:strict
    /permissive-
    /Zc:__cplusplus
    /EHsc
    /W4
    /wd4100
)

set(SRC
    src/main.cpp
    src/BluetoothRfcommClient.cpp
    src/MediaPlaybackWatcher.cpp
    src/AirPodsConnector.cpp
    src/HandoverController.cpp
    src/TrayIcon.cpp
    src/SettingsStore.cpp
    src/TeamsCallWatcher.cpp
    src/PeerRegistry.cpp
    src/BleProximityWatcher.cpp
    src/TraceUploader.cpp
    src/PlaybackSilencer.cpp
    src/v2/PeerV2Json.cpp
    src/v2/PeerV2Frame.cpp
    src/v2/PeerV2Identity.cpp
    src/v2/PeerV2Message.cpp
    src/v2/PeerV2Mesh.cpp
    src/BtHandoverSetup.cpp
    src/PeerServer.cpp
    src/Propods.rc
)

set(HDR
    common/crossdevice_protocol.hpp
    src/BluetoothRfcommClient.hpp
    src/PeerDialPolicy.hpp
    src/ProximityRules.hpp
    src/MediaPlaybackWatcher.hpp
    src/AirPodsConnector.hpp
    src/HandoverController.hpp
    src/TrayIcon.hpp
    src/SettingsStore.hpp
    src/TeamsCallWatcher.hpp
    src/PeerRegistry.hpp
    src/PeerServer.hpp
    src/BleProximityWatcher.hpp
    src/TraceUploader.hpp
    src/PlaybackSilencer.hpp
    src/BtHandoverSetup.hpp
    src/Logger.hpp
    src/v2/PeerV2Arbitration.hpp
)

add_executable(Propods WIN32 ${SRC} ${HDR})

target_include_directories(Propods PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/common
)

target_link_libraries(Propods PRIVATE
    windowsapp.lib
    Shell32.lib
    User32.lib
    Gdi32.lib
    Ole32.lib
    Shlwapi.lib
    Bthprops.lib
)

# Peer protocol v2 building blocks: unit tests, run in CI (no Bluetooth needed).
add_executable(peer_v2_tests
    tools/peer_v2_tests.cpp
    src/v2/PeerV2Json.cpp
    src/v2/PeerV2Frame.cpp
    src/v2/PeerV2Identity.cpp
    src/v2/PeerV2Message.cpp
)
target_include_directories(peer_v2_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(peer_v2_tests PRIVATE bcrypt.lib)
