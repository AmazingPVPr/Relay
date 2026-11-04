# Run the existing generated speech contract independently against SpeexDSP.
# This keeps Linux's WebRTC default and verifies the Windows/portable backend.
pkg_check_modules(SPEEX_TEST QUIET IMPORTED_TARGET speexdsp)
if(SPEEX_TEST_FOUND)
    qt_add_executable(audio_processing_speex_test
        tests/audio_processing_test.cpp src/AudioProcessor.cpp
        src/AppSettings.cpp src/AppSettings.h)
    target_include_directories(audio_processing_speex_test PRIVATE src)
    target_compile_definitions(audio_processing_speex_test PRIVATE RELAY_USE_SPEEX=1)
    target_link_libraries(audio_processing_speex_test PRIVATE Qt6::Core Qt6::Test
        PkgConfig::RNNOISE PkgConfig::SPEEX_TEST)
    add_test(NAME audio_processing_speex_test COMMAND audio_processing_speex_test)
    set_tests_properties(audio_processing_speex_test PROPERTIES TIMEOUT 45)
endif()
