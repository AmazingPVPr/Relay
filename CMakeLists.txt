cmake_minimum_required(VERSION 3.24)
# Refuse local configuration before project() can detect or run a compiler.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.relay-runtime-only" OR
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../.relay-runtime-only")
    message(FATAL_ERROR "This Relay copy is runtime-only. All development, builds and tests run on Vault; use the Vault development workflow.")
endif()
project(Relay VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)
option(RELAY_REQUIRE_DAVE "Require encrypted Discord voice support" ON)
option(RELAY_INSTALL_MEDIA_HELPER "Install a matching platform media helper" ON)
option(RELAY_DEPLOY_QT "Deploy Qt runtime and QML imports for portable packages" OFF)
if(WIN32)
    set(RELAY_AUDIO_PROCESSING_DEFAULT speex)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(RELAY_AUDIO_PROCESSING_DEFAULT webrtc)
else()
    message(FATAL_ERROR "Relay currently supports Linux and Windows desktop builds.")
endif()
set(RELAY_AUDIO_PROCESSING "${RELAY_AUDIO_PROCESSING_DEFAULT}" CACHE STRING "Echo cancellation/AGC backend: webrtc or speex")
set_property(CACHE RELAY_AUDIO_PROCESSING PROPERTY STRINGS webrtc speex)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Network WebSockets Qml Quick QuickControls2 Multimedia Concurrent Svg)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
endif()
if(WIN32)
    find_package(Qt6 REQUIRED COMPONENTS Widgets)
    set(RELAY_DESKTOP_SOURCES
        src/platform/WindowsGlobalShortcuts.cpp src/platform/WindowsGlobalShortcuts.h
        src/platform/WindowsNotificationManager.cpp src/platform/WindowsNotificationManager.h
        src/platform/WindowsSystemAudioCapture.cpp)
else()
    find_package(Qt6 REQUIRED COMPONENTS DBus)
    set(RELAY_DESKTOP_SOURCES src/NotificationManager.cpp src/control/GlobalShortcuts.cpp src/SystemAudioCapture.cpp)
endif()
find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Qt6Keychain CONFIG REQUIRED)
pkg_check_modules(QRENCODE REQUIRED IMPORTED_TARGET libqrencode)
pkg_check_modules(AV REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)
if(NOT WIN32)
    pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse)
endif()
pkg_check_modules(RNNOISE REQUIRED IMPORTED_TARGET rnnoise)
if(RELAY_AUDIO_PROCESSING STREQUAL "webrtc")
    pkg_check_modules(AUDIO_PROCESSING REQUIRED IMPORTED_TARGET webrtc-audio-processing-1)
elseif(RELAY_AUDIO_PROCESSING STREQUAL "speex")
    pkg_check_modules(AUDIO_PROCESSING REQUIRED IMPORTED_TARGET speexdsp)
else()
    message(FATAL_ERROR "RELAY_AUDIO_PROCESSING must be webrtc or speex")
endif()
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
find_path(DAVE_INCLUDE_DIR dave/dave.h HINTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libdave/include")
find_library(DAVE_LIBRARY NAMES dave libdave HINTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libdave/lib")
qt_standard_project_setup(REQUIRES 6.8)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libdatachannel-source/CMakeLists.txt")
    set(RELAY_DATACHANNEL_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libdatachannel-source")
else()
    set(RELAY_DATACHANNEL_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/../../work/deps/libdatachannel-source")
endif()
set(RELAY_DATACHANNEL_SOURCE "${RELAY_DATACHANNEL_DEFAULT}" CACHE PATH "Pinned libdatachannel source directory")
if(NOT EXISTS "${RELAY_DATACHANNEL_SOURCE}/CMakeLists.txt")
    message(FATAL_ERROR "Provide pinned libdatachannel sources with RELAY_DATACHANNEL_SOURCE, or fetch them through the Vault executor.")
endif()
set(NO_MEDIA ON CACHE BOOL "No WebRTC media" FORCE)
set(NO_WEBSOCKET ON CACHE BOOL "Use Qt WebSockets for setup" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
add_subdirectory("${RELAY_DATACHANNEL_SOURCE}" "relay-datachannel" EXCLUDE_FROM_ALL)


add_library(relay_core STATIC
    src/addons/AddonIdentityBroker.cpp src/addons/AddonIdentityBroker.h
    ${RELAY_DESKTOP_SOURCES}
    src/RelayDiscordBridge.cpp src/RelayDiscordBridge.h
    src/RelayStreams.cpp src/RelayStreams.h
    src/RelayPeers.cpp src/RelayPeers.h src/RelayPeerTransport.cpp src/RelayPeerTransport.h src/RelayMcp.cpp
    src/AppSettings.cpp src/AppSettings.h
    src/ServerLayout.cpp src/ServerLayout.h
    src/AccountSettingsService.cpp src/AccountSettingsService.h
    src/ChatText.cpp src/ChatText.h
    src/MessageReferences.cpp src/MessageReferences.h
    src/GifSearch.cpp src/GifSearch.h
    src/VideoResolver.cpp src/VideoResolver.h src/InlineMedia.h
    src/NativeMediaSource.cpp src/NativeMediaSource.h
    src/TranslationService.cpp src/TranslationService.h
    src/ChannelTools.cpp src/ChannelTools.h
    src/ForumTools.cpp src/ForumTools.h
    src/MemberActions.cpp src/MemberActions.h
    src/GuildManagement.cpp src/GuildManagement.h
    src/CredentialStore.cpp src/CredentialStore.h src/CredentialStatus.h
    src/AccountSwitcher.cpp src/AccountSwitcher.h
    src/ContextActions.cpp src/ContextActions.h
    src/AttachmentUpload.cpp src/AttachmentUpload.h
    src/BroadcastEngine.cpp src/BroadcastEngine.h
    src/BroadcastTransport.cpp src/BroadcastTransport.h
    src/NotificationManager.h
    src/SoundEffects.cpp src/SoundEffects.h
    src/Soundboard.cpp src/Soundboard.h
    src/AudioProcessor.cpp src/AudioProcessor.h
    src/control/ControlServer.cpp src/control/ControlServer.h
    src/control/GlobalShortcuts.h
    src/control/ShortcutSettings.cpp src/control/ShortcutSettings.h
    src/RecordModel.cpp src/RecordModel.h
    src/VoiceRoster.cpp src/VoiceRoster.h
    src/DiscordClient.cpp src/DiscordClient.h
    src/DiscordAuth.cpp src/DiscordAuth.h
    src/AuthCrypto.cpp src/AuthCrypto.h
    src/VoiceEngine.cpp src/VoiceEngine.h
    src/CameraEngine.cpp src/CameraEngine.h
    src/VoicePacket.cpp src/VoicePacket.h
    src/VideoPacket.cpp src/VideoPacket.h
    src/BroadcastPacket.cpp src/BroadcastPacket.h src/BroadcastPacer.h src/BroadcastRepairBudget.h
    src/H264Bitstream.cpp src/H264Bitstream.h
    src/BroadcastEncoder.cpp src/BroadcastEncoder.h
    src/SystemAudioCapture.h
    src/StreamEngine.cpp src/StreamEngine.h
    src/StreamDecoder.cpp src/StreamDecoder.h
    src/addons/AddonManager.cpp src/addons/AddonManager.h src/addons/AddonInterface.h src/addons/ShareSourceInterface.h
    src/addons/AddonBroadcastBridge.cpp src/addons/AddonBroadcastBridge.h src/addons/BroadcastSourceInterface.h)
target_include_directories(relay_core PUBLIC src)
target_link_libraries(relay_core PUBLIC datachannel-static)
target_link_libraries(relay_core PUBLIC Qt6::Core Qt6::Gui Qt6::Network Qt6::WebSockets Qt6::Multimedia
    Qt6::Qml Qt6Keychain::Qt6Keychain PkgConfig::AV PkgConfig::RNNOISE PkgConfig::AUDIO_PROCESSING PkgConfig::OPUS PkgConfig::SODIUM PkgConfig::QRENCODE OpenSSL::Crypto)
if(WIN32)
    target_link_libraries(relay_core PUBLIC Qt6::Widgets ole32 uuid user32)
    target_compile_definitions(relay_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
else()
    target_link_libraries(relay_core PUBLIC Qt6::DBus PkgConfig::PULSE)
endif()
if(RELAY_AUDIO_PROCESSING STREQUAL "speex")
    target_compile_definitions(relay_core PRIVATE RELAY_USE_SPEEX=1)
endif()
if(MSVC)
    target_compile_options(relay_core PRIVATE /W4 /utf-8)
else()
    target_compile_options(relay_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(DAVE_INCLUDE_DIR AND DAVE_LIBRARY)
    target_include_directories(relay_core PRIVATE "${DAVE_INCLUDE_DIR}")
    target_link_libraries(relay_core PUBLIC "${DAVE_LIBRARY}")
    target_compile_definitions(relay_core PRIVATE RELAY_HAS_DAVE=1)
elseif(RELAY_REQUIRE_DAVE)
    message(FATAL_ERROR "libdave is required. Build pinned dependencies first or set DAVE_INCLUDE_DIR and DAVE_LIBRARY.")
else()
    message(WARNING "libdave missing: encrypted Discord voice will be unavailable")
endif()

add_library(relay_obs MODULE addons/obs/ObsAddon.cpp src/StudioBridge.cpp src/StudioBridge.h src/ObsInputStore.cpp src/ObsInputStore.h)
target_include_directories(relay_obs PRIVATE src)
target_link_libraries(relay_obs PRIVATE Qt6::Core Qt6::Gui Qt6::Multimedia Qt6Keychain::Qt6Keychain PkgConfig::AV)
set_target_properties(relay_obs PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addons")
qt_add_resources(relay_obs obs_settings PREFIX "/relay/addons/org.relay.obs"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/addons/obs" FILES addons/obs/Settings.qml addons/obs/Input.qml)

add_library(relay_soundboard MODULE addons/soundboard/SoundboardAddon.cpp)
target_include_directories(relay_soundboard PRIVATE src)
target_link_libraries(relay_soundboard PRIVATE Qt6::Core Qt6::Gui Qt6::Multimedia)
set_target_properties(relay_soundboard PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addons")
qt_add_resources(relay_soundboard soundboard_settings PREFIX "/relay/addons/org.relay.soundboard"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/addons/soundboard" FILES addons/soundboard/Settings.qml addons/soundboard/Board.qml)

add_library(relay_hiddenchannels MODULE addons/hiddenchannels/HiddenChannelsAddon.cpp)
target_include_directories(relay_hiddenchannels PRIVATE src)
target_link_libraries(relay_hiddenchannels PRIVATE Qt6::Core)
set_target_properties(relay_hiddenchannels PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addons")
qt_add_resources(relay_hiddenchannels hiddenchannels_settings PREFIX "/relay/addons/org.relay.hiddenchannels"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/addons/hiddenchannels" FILES addons/hiddenchannels/Settings.qml)

add_library(relay_rendezvous_core STATIC src/RelayRendezvous.cpp src/RelayRendezvous.h)
target_link_libraries(relay_rendezvous_core PUBLIC Qt6::Core Qt6::Network Qt6::WebSockets PkgConfig::SODIUM)
target_include_directories(relay_rendezvous_core PUBLIC src)
qt_add_executable(relay-rendezvous src/relay-rendezvous-main.cpp)
target_link_libraries(relay-rendezvous PRIVATE relay_rendezvous_core)

qt_add_executable(relay src/RelayUpdater.h src/ImageEditor.cpp src/ImageEditor.h src/DiagnosticLog.h src/WindowLifecycleProbe.cpp src/WindowLifecycleProbe.h src/main.cpp src/SoftwareVideo.cpp src/SoftwareVideo.h src/CrispImage.cpp src/CrispImage.h)
add_dependencies(relay relay_obs relay_soundboard relay_hiddenchannels)
if(WIN32)
    enable_language(RC)
    target_sources(relay PRIVATE assets/relay.rc)
    target_include_directories(relay PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/assets")
    set_target_properties(relay PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
file(GLOB RELAY_SOUNDS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/sounds/*.wav")
qt_add_resources(relay relay_sounds PREFIX "/relay/sounds" BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets/sounds" FILES ${RELAY_SOUNDS})
target_link_libraries(relay PRIVATE relay_core Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Concurrent Qt6::Svg)
file(GLOB_RECURSE RELAY_QML CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/qml/*.qml" "${CMAKE_CURRENT_SOURCE_DIR}/qml/*.js")
foreach(qml_file IN LISTS RELAY_QML)
    file(RELATIVE_PATH relative_qml "${CMAKE_CURRENT_SOURCE_DIR}/qml" "${qml_file}")
    set_source_files_properties("${qml_file}" PROPERTIES QT_RESOURCE_ALIAS "${relative_qml}")
endforeach()
qt_add_qml_module(relay URI Relay VERSION 1.0 RESOURCE_PREFIX "/" QML_FILES ${RELAY_QML})
qt_add_resources(relay relay_icon PREFIX "/relay/icons" BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets" FILES assets/relay.svg)
foreach(addon IN ITEMS relay_obs relay_soundboard relay_hiddenchannels)
    set_target_properties(${addon} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addons")
endforeach()
set_target_properties(relay PROPERTIES BUILD_RPATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libdave/lib" INSTALL_RPATH "$ORIGIN/../lib")

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME updater_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/updater_test.py)
    add_library(relay_addon_fixture MODULE addons/testfixture/DummyAddon.cpp)
    target_include_directories(relay_addon_fixture PRIVATE src)
    target_link_libraries(relay_addon_fixture PRIVATE Qt6::Core Qt6::Multimedia)
    set_target_properties(relay_addon_fixture PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test-addons")
    qt_add_resources(relay_addon_fixture fixture_settings PREFIX "/relay/addons/org.relay.fixture"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/addons/testfixture" FILES addons/testfixture/Settings.qml addons/testfixture/Input.qml)
    add_library(relay_window_fixture MODULE addons/testfixture/WindowAddon.cpp)
    target_include_directories(relay_window_fixture PRIVATE src)
    target_link_libraries(relay_window_fixture PRIVATE Qt6::Core)
    set_target_properties(relay_window_fixture PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/window-test-addons")
    file(GLOB RELAY_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*test.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_tests.cpp")
    include(tests/speech_backends.cmake)
    foreach(test_source IN LISTS RELAY_TESTS)
        get_filename_component(test_name "${test_source}" NAME_WE)
        if(NOT WIN32 AND test_name STREQUAL "windows_platform_test")
            continue()
        endif()
        if(WIN32 AND (test_name STREQUAL "global_shortcuts_test" OR test_name STREQUAL "notification_test"))
            continue()
        endif()
        qt_add_executable(${test_name} "${test_source}")
        if(WIN32 AND test_name STREQUAL "windows_platform_test")
            qt_add_resources(${test_name} platform_icon PREFIX "/relay/icons"
                BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets" FILES assets/relay.svg)
        endif()
        target_link_libraries(${test_name} PRIVATE relay_core Qt6::Test)
        if(test_name STREQUAL "relay_peers_test")
            target_link_libraries(${test_name} PRIVATE relay_rendezvous_core)
        endif()
        if(test_name STREQUAL "relay_peer_ui_test")
            target_sources(${test_name} PRIVATE src/SoftwareVideo.cpp src/SoftwareVideo.h)
            target_link_libraries(${test_name} PRIVATE Qt6::Qml Qt6::Quick Qt6::QuickControls2)
            target_compile_definitions(${test_name} PRIVATE RELAY_PEER_UI_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/qml/components/RelayConversation.qml")
        endif()
        if(test_name STREQUAL "window_addon_test")
            add_dependencies(${test_name} relay_window_fixture)
            target_compile_definitions(${test_name} PRIVATE RELAY_WINDOW_FIXTURE="$<TARGET_FILE:relay_window_fixture>")
        endif()
        if(test_name STREQUAL "window_lifecycle_test")
            add_dependencies(${test_name} relay)
            target_compile_definitions(${test_name} PRIVATE RELAY_TEST_APPLICATION_PATH="$<TARGET_FILE:relay>")
        endif()
        if(test_name STREQUAL "addon_test" OR test_name STREQUAL "share_source_ui_test")
            add_dependencies(${test_name} relay_addon_fixture)
            target_compile_definitions(${test_name} PRIVATE RELAY_TEST_ADDON_PATH="$<TARGET_FILE:relay_addon_fixture>")
        endif()
        if(test_name STREQUAL "addon_broadcast_test")
            add_dependencies(${test_name} relay_obs relay_addon_fixture)
            target_compile_definitions(${test_name} PRIVATE RELAY_TEST_OBS_ADDON_PATH="$<TARGET_FILE:relay_obs>" RELAY_TEST_ADDON_PATH="$<TARGET_FILE:relay_addon_fixture>")
        endif()
        if(test_name STREQUAL "message_menu_ui_test" OR test_name STREQUAL "crisp_image_test" OR test_name STREQUAL "soundboard_volume_ui_test")
            target_sources(${test_name} PRIVATE src/CrispImage.cpp src/CrispImage.h)
            target_link_libraries(${test_name} PRIVATE Qt6::Quick Qt6::Concurrent)
        endif()
        if(test_name STREQUAL "image_editor_test")
            target_compile_definitions(${test_name} PRIVATE RELAY_IMAGE_EDITOR_QML="${CMAKE_CURRENT_SOURCE_DIR}/qml/components/ImageEditPanel.qml")
        endif()
        if(test_name STREQUAL "image_editor_test" OR test_name STREQUAL "message_menu_ui_test" OR test_name STREQUAL "animated_image_test")
            target_sources(${test_name} PRIVATE src/ImageEditor.cpp src/ImageEditor.h)
            target_link_libraries(${test_name} PRIVATE Qt6::Quick Qt6::QuickControls2)
        endif()
        if(test_name STREQUAL "message_menu_ui_test" OR test_name STREQUAL "software_video_test")
            target_sources(${test_name} PRIVATE src/SoftwareVideo.cpp src/SoftwareVideo.h)
            target_link_libraries(${test_name} PRIVATE Qt6::Quick)
        endif()
        if(test_name STREQUAL "message_references_test" OR test_name STREQUAL "message_menu_ui_test" OR test_name STREQUAL "search_ui_test" OR test_name STREQUAL "soundboard_volume_ui_test" OR test_name STREQUAL "share_source_ui_test" OR test_name STREQUAL "scroll_test" OR test_name STREQUAL "history_viewport_test" OR test_name STREQUAL "server_rail_test" OR test_name STREQUAL "animated_image_test" OR test_name STREQUAL "read_receipt_tracker_test" OR test_name STREQUAL "clipboard_composer_test")
            target_link_libraries(${test_name} PRIVATE Qt6::Qml Qt6::Quick Qt6::QuickControls2)
        endif()
        if(test_name STREQUAL "sound_effects_test")
            qt_add_resources(${test_name} test_sounds PREFIX "/relay/sounds" BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets/sounds" FILES ${RELAY_SOUNDS})
        endif()
        if(test_name STREQUAL "custom_soundboard_test")
            add_dependencies(${test_name} relay_soundboard)
            target_compile_definitions(${test_name} PRIVATE RELAY_SOUNDBOARD_ADDON_PATH="$<TARGET_FILE:relay_soundboard>")
        endif()
        if(test_name STREQUAL "diagnostic_log_test")
            target_sources(${test_name} PRIVATE src/DiagnosticLog.h)
        endif()
        if(test_name STREQUAL "studio_test")
            target_sources(${test_name} PRIVATE src/StudioBridge.cpp src/StudioBridge.h src/ObsInputStore.cpp src/ObsInputStore.h)
            target_link_libraries(${test_name} PRIVATE PkgConfig::AV)
        endif()
        if(test_name STREQUAL "broadcast_gateway_test")
            target_sources(${test_name} PRIVATE src/StudioBridge.cpp src/StudioBridge.h src/ObsInputStore.cpp src/ObsInputStore.h)
        endif()
        if(test_name STREQUAL "obs_input_store_test")
            target_sources(${test_name} PRIVATE src/ObsInputStore.cpp src/ObsInputStore.h)
        endif()
        if((test_name STREQUAL "voice_gateway_test" OR test_name STREQUAL "voice_playout_test" OR test_name STREQUAL "stream_gateway_test" OR test_name STREQUAL "broadcast_gateway_test" OR test_name STREQUAL "h264_bitstream_test") AND DAVE_INCLUDE_DIR AND DAVE_LIBRARY)
            target_compile_definitions(${test_name} PRIVATE RELAY_TEST_HAS_DAVE=1)
            target_include_directories(${test_name} PRIVATE "${DAVE_INCLUDE_DIR}")
            if(WIN32)
                find_package(MLSPP CONFIG REQUIRED)
                target_link_libraries(${test_name} PRIVATE MLSPP::mlspp)
            endif()
            find_path(RELAY_MLSPP_TEST_INCLUDE_DIR mls/messages.h HINTS "${CMAKE_CURRENT_SOURCE_DIR}/../../work/voice-deps/include/mlspp")
            if(RELAY_MLSPP_TEST_INCLUDE_DIR)
                target_include_directories(${test_name} PRIVATE "${RELAY_MLSPP_TEST_INCLUDE_DIR}")
                target_compile_definitions(${test_name} PRIVATE RELAY_TEST_MLSPP=1)
                target_link_libraries(${test_name} PRIVATE OpenSSL::Crypto)
            endif()
        endif()
        add_test(NAME ${test_name} COMMAND ${test_name})
        set_tests_properties(${test_name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 45)
        if(test_name STREQUAL "broadcast_gateway_test")
            # Includes sustained source, pacing, sender clock, and MLS recovery cases.
            # The fixture already injects GUI load and runs both encrypted media
            # endpoints; reserve the bounded executor for its timing assertions.
            set_tests_properties(${test_name} PROPERTIES TIMEOUT 240 RUN_SERIAL TRUE)
        endif()
        if(test_name STREQUAL "studio_test")
            # Includes real-time hidden-preview, bounded GOP replay and fallback cases.
            set_tests_properties(${test_name} PROPERTIES TIMEOUT 150 RUN_SERIAL TRUE)
        endif()
        if(test_name STREQUAL "voice_gateway_test" OR test_name STREQUAL "voice_playout_test")
            # Includes generated real-time capture, MLS and bounded reconnect scenarios.
            set_tests_properties(${test_name} PROPERTIES TIMEOUT 90)
        endif()
        if(test_name STREQUAL "voice_playout_test")
            # Its generated 20 ms playout deadline must not compete with other
            # test processes encoding/decoding both ends of an OBS stream.
            # Internal UI-stall cases and zero-loss assertions remain enabled.
            set_tests_properties(${test_name} PROPERTIES RUN_SERIAL TRUE)
        endif()
        if(NOT WIN32 AND (test_name MATCHES "^voice_.*test$" OR test_name STREQUAL "soundboard_test" OR test_name STREQUAL "soundboard_volume_ui_test"))
            # These fixtures inject PCM and never open a host audio device.
            set_tests_properties(${test_name} PROPERTIES ENVIRONMENT
                "QT_QPA_PLATFORM=offscreen;QT_AUDIO_BACKEND=pulseaudio;PULSE_SERVER=unix:/tmp/relay-qa-no-audio.sock;PIPEWIRE_REMOTE=relay-qa-no-pipewire")
        endif()
    endforeach()
endif()

install(TARGETS relay relay-rendezvous RUNTIME DESTINATION bin)
install(FILES docs/relay-private-messaging.md DESTINATION share/relay)
install(DIRECTORY third_party/libdatachannel/licenses/ DESTINATION share/relay/licenses/datachannel)
install(TARGETS relay_obs relay_soundboard relay_hiddenchannels
    LIBRARY DESTINATION lib/relay/addons RUNTIME DESTINATION bin/addons)
if(WIN32 AND RELAY_DEPLOY_QT)
    qt_generate_deploy_qml_app_script(TARGET relay OUTPUT_SCRIPT relay_deploy_script)
    install(SCRIPT ${relay_deploy_script})
endif()
if(RELAY_INSTALL_MEDIA_HELPER AND WIN32)
    install(PROGRAMS tools/yt-dlp.exe DESTINATION bin OPTIONAL)
elseif(RELAY_INSTALL_MEDIA_HELPER)
    install(PROGRAMS tools/yt-dlp DESTINATION libexec/relay OPTIONAL)
endif()
install(FILES LICENSE DESTINATION share/relay/licenses)
install(FILES docs/PORTABILITY.md DESTINATION share/relay)
install(FILES tools/yt-dlp.LICENSE tools/yt-dlp-ejs.LICENSE tools/yt-dlp.metadata.json
    DESTINATION share/relay/licenses OPTIONAL)
if(DAVE_LIBRARY AND NOT WIN32)
    install(FILES "${DAVE_LIBRARY}" DESTINATION lib)
endif()
install(DIRECTORY third_party/libdave/licenses/ DESTINATION share/relay/licenses OPTIONAL)
install(FILES assets/relay.desktop DESTINATION share/applications)
install(FILES assets/relay.svg DESTINATION share/icons/hicolor/scalable/apps)
