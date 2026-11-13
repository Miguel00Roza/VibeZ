cmake_minimum_required(VERSION 3.25)

project(VibeZ VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

if(NOT WIN32)
    message(FATAL_ERROR "The initial VibeZ media platform targets Windows. Network/protocol code is portable by design.")
endif()

# The manifest install is kept inside the repository so a clean clone does not
# depend on a machine-wide vcpkg integration.
list(PREPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/x64-windows")
find_package(Opus CONFIG REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_library(vibez_protocol
    common/protocol/Protocol.cpp
)
target_include_directories(vibez_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(vibez_protocol PUBLIC cxx_std_20)

add_library(vibez_audio
    client/audio/AudioFormat.cpp
    client/audio/AudioMixer.cpp
    client/audio/OpusCodec.cpp
    client/audio/WasapiCapture.cpp
    client/audio/WasapiDevices.cpp
    client/audio/WasapiPlayback.cpp
    client/audio/VoiceEngine.cpp
)
target_include_directories(vibez_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vibez_audio PUBLIC Opus::opus ole32 uuid mmdevapi propsys avrt)
target_compile_options(vibez_audio PRIVATE /W4 /permissive- /Zc:__cplusplus)

add_library(vibez_net
    client/net/NetworkClient.cpp
)
target_include_directories(vibez_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vibez_net PUBLIC vibez_protocol ws2_32)
target_compile_options(vibez_net PRIVATE /W4 /permissive- /Zc:__cplusplus)

add_library(vibez_client_core
    client/core/ClientSession.cpp
)
target_include_directories(vibez_client_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vibez_client_core PUBLIC vibez_net vibez_audio)
target_compile_options(vibez_client_core PRIVATE /W4 /permissive- /Zc:__cplusplus)

# Qt adapter and main window. Only this layer depends on Qt; the core stays
# usable by the console client and tests.
add_library(vibez_ui
    client/ui/SessionController.cpp
    client/ui/SessionController.hpp
    client/ui/MainWindow.cpp
    client/ui/MainWindow.hpp
)
target_include_directories(vibez_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vibez_ui PUBLIC vibez_client_core Qt6::Widgets)
target_compile_options(vibez_ui PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8)


add_library(vibez_server_lib
    server/src/Server.cpp
    server/src/ScreenStream.cpp
    server/src/ScreenStream.hpp
    server/src/SystemAudio.cpp
    server/src/SystemAudio.hpp
    server/src/WebServer.cpp
    server/src/WebServer.hpp
    server/src/WebPage.hpp
)
target_include_directories(vibez_server_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vibez_server_lib PUBLIC vibez_protocol ws2_32 iphlpapi PRIVATE Opus::opus)

# Reading/writing vibez-server.ini. Kept out of the server library so the tests
# can exercise it without opening sockets.
add_library(vibez_server_config
    server/src/ServerConfig.cpp
    server/src/ServerConfig.hpp
)
target_include_directories(vibez_server_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(vibez_server_config PRIVATE /W4 /permissive- /Zc:__cplusplus)

add_executable(VibeZServer server/src/main.cpp server/VibeZServer.rc)
target_link_libraries(VibeZServer PRIVATE vibez_server_lib vibez_server_config)

# The console client is kept for scripting and headless tests.
add_executable(VibeZConsole client/src/main.cpp)
set_target_properties(VibeZConsole PROPERTIES OUTPUT_NAME "VibeZConsole")
target_link_libraries(VibeZConsole PRIVATE vibez_client_core)

# GUI client. VibeZ.exe is the user-facing application.
add_executable(VibeZClient WIN32 client/ui/main.cpp client/ui/VibeZ.rc)
set_target_properties(VibeZClient PROPERTIES OUTPUT_NAME "VibeZ")
target_link_libraries(VibeZClient PRIVATE vibez_ui)
target_compile_options(VibeZClient PRIVATE /utf-8)

function(copy_opus_runtime target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Opus::opus>"
            "$<TARGET_FILE_DIR:${target}>"
        VERBATIM)
endfunction()

# Copies the Qt plugins (platform, image formats, styles) beside the GUI
# binary. vcpkg's windeployqt locates the release prefix even in a Debug build
# and fails to find the debug Qt DLLs, so copy the plugin trees explicitly.
function(deploy_qt_runtime target)
    if(NOT WIN32)
        return()
    endif()
    set(_debug_plugins "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/x64-windows/debug/Qt6/plugins")
    set(_release_plugins "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/x64-windows/Qt6/plugins")
    if(EXISTS "${_debug_plugins}" AND EXISTS "${_release_plugins}")
        set(_plugins "$<IF:$<CONFIG:Debug>,${_debug_plugins},${_release_plugins}>")
    elseif(EXISTS "${_debug_plugins}")
        set(_plugins "${_debug_plugins}")
    elseif(EXISTS "${_release_plugins}")
        set(_plugins "${_release_plugins}")
    else()
        message(WARNING "Qt plugins not found; add the Qt plugin directory to the vcpkg install")
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${_plugins}" "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Deploying Qt plugins for ${target}"
        VERBATIM)
endfunction()

copy_opus_runtime(VibeZConsole)
copy_opus_runtime(VibeZClient)
copy_opus_runtime(VibeZServer)

# Qt's imported targets expose their runtime DLLs to CMake.  Copying this
# closure complements the plugin deployment above and includes libraries such
# as zlib, FreeType and ICU from vcpkg.  Both the release and the debug bin
# directories are copied because debug Qt DLLs depend on the "d"-suffixed
# third-party libraries (libpng16d, freetyped, zd, pcre2-16d, ...).
file(GLOB VIBEZ_VCPKG_RUNTIME_DLLS
    "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/x64-windows/bin/*.dll"
    "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/x64-windows/debug/bin/*.dll")
add_custom_command(TARGET VibeZClient POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy -t "$<TARGET_FILE_DIR:VibeZClient>"
        $<TARGET_RUNTIME_DLLS:VibeZClient>
    COMMAND_EXPAND_LISTS
    VERBATIM)
add_custom_command(TARGET VibeZClient POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${VIBEZ_VCPKG_RUNTIME_DLLS} "$<TARGET_FILE_DIR:VibeZClient>"
    COMMAND_EXPAND_LISTS
    VERBATIM)
deploy_qt_runtime(VibeZClient)


include(CTest)
if(BUILD_TESTING)
    add_executable(vibez_protocol_tests tests/ProtocolTests.cpp)
    target_link_libraries(vibez_protocol_tests PRIVATE vibez_protocol)
    add_test(NAME vibez_protocol_tests COMMAND vibez_protocol_tests)

    add_executable(vibez_audio_tests tests/AudioTests.cpp)
    target_link_libraries(vibez_audio_tests PRIVATE vibez_audio)
    target_compile_options(vibez_audio_tests PRIVATE /W4 /permissive- /Zc:__cplusplus)
    copy_opus_runtime(vibez_audio_tests)
    add_test(NAME vibez_audio_tests COMMAND vibez_audio_tests)

    add_executable(vibez_audio_device_tests tests/AudioDeviceTests.cpp)
    target_link_libraries(vibez_audio_device_tests PRIVATE vibez_audio)
    target_compile_options(vibez_audio_device_tests PRIVATE /W4 /permissive- /Zc:__cplusplus)
    copy_opus_runtime(vibez_audio_device_tests)
    add_test(NAME vibez_audio_device_tests COMMAND vibez_audio_device_tests)
    set_tests_properties(vibez_audio_device_tests PROPERTIES TIMEOUT 30)

    add_executable(vibez_server_integration_tests tests/ServerIntegrationTests.cpp)
    target_link_libraries(vibez_server_integration_tests PRIVATE vibez_server_lib)
    copy_opus_runtime(vibez_server_integration_tests)
    add_test(NAME vibez_server_integration_tests COMMAND vibez_server_integration_tests)

    add_executable(vibez_server_config_tests tests/ServerConfigTests.cpp)
    target_link_libraries(vibez_server_config_tests PRIVATE vibez_server_config)
    target_compile_options(vibez_server_config_tests PRIVATE /W4 /permissive- /Zc:__cplusplus)
    add_test(NAME vibez_server_config_tests COMMAND vibez_server_config_tests)

    add_executable(vibez_network_tests tests/NetworkTests.cpp)
    target_link_libraries(vibez_network_tests PRIVATE vibez_net vibez_server_lib)
    target_compile_options(vibez_network_tests PRIVATE /W4 /permissive- /Zc:__cplusplus)
    copy_opus_runtime(vibez_network_tests)
    add_test(NAME vibez_network_tests COMMAND vibez_network_tests)

    add_executable(vibez_voice_network_tests tests/VoiceOverNetworkTests.cpp)
    target_link_libraries(vibez_voice_network_tests PRIVATE vibez_net vibez_audio vibez_server_lib)
    target_compile_options(vibez_voice_network_tests PRIVATE /W4 /permissive- /Zc:__cplusplus)
    copy_opus_runtime(vibez_voice_network_tests)
    add_test(NAME vibez_voice_network_tests COMMAND vibez_voice_network_tests)

    add_executable(vibez_web_tests tests/WebTests.cpp)
    target_link_libraries(vibez_web_tests PRIVATE vibez_server_lib Opus::opus)
    target_compile_options(vibez_web_tests PRIVATE /W4 /permissive- /Zc:__cplusplus)
    copy_opus_runtime(vibez_web_tests)
    add_test(NAME vibez_web_tests COMMAND vibez_web_tests)

    set_tests_properties(vibez_protocol_tests vibez_audio_tests vibez_server_integration_tests vibez_server_config_tests vibez_network_tests vibez_voice_network_tests vibez_web_tests PROPERTIES TIMEOUT 60)
endif()

install(TARGETS VibeZClient VibeZConsole VibeZServer RUNTIME DESTINATION .)