cmake_minimum_required(VERSION 3.24)
project(RelayAudioBackendRegression LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.12 REQUIRED COMPONENTS Core Multimedia)
get_filename_component(RELAY_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
add_executable(audio_backend_probe probe.cpp
    "${RELAY_SOURCE_DIR}/src/SoundEffects.cpp" "${RELAY_SOURCE_DIR}/src/SoundEffects.h"
    "${RELAY_SOURCE_DIR}/src/AppSettings.cpp" "${RELAY_SOURCE_DIR}/src/AppSettings.h")
target_include_directories(audio_backend_probe PRIVATE "${RELAY_SOURCE_DIR}/src")
target_link_libraries(audio_backend_probe PRIVATE Qt6::Core Qt6::Multimedia)
file(GLOB CUES "${RELAY_SOURCE_DIR}/assets/sounds/*.wav")
qt_add_resources(audio_backend_probe cues PREFIX "/relay/sounds"
    BASE "${RELAY_SOURCE_DIR}/assets/sounds" FILES ${CUES})
