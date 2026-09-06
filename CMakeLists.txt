cmake_minimum_required(VERSION 3.21)

# ServerCore selects the MSVC runtime through the target property introduced by
# CMP0091. The policy must be selected before C++ is enabled by project().
cmake_policy(SET CMP0091 NEW)

project(SummitServer
        VERSION 0.1.0
        DESCRIPTION "ServerCore based multiplayer relay for Summit"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SERVERCORE_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../ServerCore"
    CACHE PATH "Path to the ServerCore source tree")

if (NOT EXISTS "${SERVERCORE_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
            "ServerCore was not found at '${SERVERCORE_SOURCE_DIR}'. "
            "Set SERVERCORE_SOURCE_DIR to its source directory.")
endif ()

# SummitServer consumes ServerCore as a library; its standalone tests default to OFF here.
set(SERVERCORE_BUILD_TESTS OFF CACHE BOOL "Build ServerCore tests")
add_subdirectory("${SERVERCORE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/ServerCore")

add_library(SummitServerBackend STATIC
        SummitServerBackend.cpp
        SummitServerBackend.h
        SummitMovementTransport.h
        SummitUdpTransport.cpp
        SummitUdpTransport.h)
target_include_directories(SummitServerBackend PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(SummitServerBackend PUBLIC ServerCore::ServerCore PRIVATE bcrypt)
target_compile_features(SummitServerBackend PUBLIC cxx_std_20)

add_executable(SummitServer main.cpp ServerConsole.cpp ServerConsole.h)
target_link_libraries(SummitServer PRIVATE SummitServerBackend)

function(summit_configure_target target_name)
    if (MSVC)
        set_property(TARGET "${target_name}" PROPERTY
                MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        target_compile_options("${target_name}" PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
    else ()
        target_compile_options("${target_name}" PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif ()
endfunction()

summit_configure_target(SummitServerBackend)
summit_configure_target(SummitServer)

# The load client is a small console executable in this same build. It uses real
# TCP/JSON messages without starting the game engine or requiring a script runtime.
option(SUMMITSERVER_BUILD_LOAD_TOOL "Build the standalone Summit load client" ${PROJECT_IS_TOP_LEVEL})
if (SUMMITSERVER_BUILD_LOAD_TOOL)
    add_executable(SummitLoadTest tests/load/SummitLoadTest.cpp)
    target_link_libraries(SummitLoadTest PRIVATE ServerCore::ServerCore)
    summit_configure_target(SummitLoadTest)
    add_custom_command(TARGET SummitLoadTest POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/StartLoadTestServer.cmd"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/RunLoadTest.cmd"
                    "$<TARGET_FILE_DIR:SummitLoadTest>"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/load/README.md"
                    "$<TARGET_FILE_DIR:SummitLoadTest>/LOAD_TEST.md"
            VERBATIM)
endif ()

option(SUMMITSERVER_BUILD_TESTS "Build SummitServer tests" ${PROJECT_IS_TOP_LEVEL})
if (SUMMITSERVER_BUILD_TESTS)
    enable_testing()
    if (TARGET SummitLoadTest)
        add_test(NAME SummitLoadToolSelfCheck COMMAND SummitLoadTest --self-check)
        set_tests_properties(SummitLoadToolSelfCheck PROPERTIES TIMEOUT 10)
    endif ()
    add_executable(SummitServerTests tests/SummitServerBackendTest.cpp)
    target_link_libraries(SummitServerTests PRIVATE SummitServerBackend)
    summit_configure_target(SummitServerTests)
    add_test(NAME SummitServerBackend COMMAND SummitServerTests)
    set_tests_properties(SummitServerBackend PROPERTIES TIMEOUT 20)

    add_executable(SummitServerAoiTests tests/SummitServerAoiTests.cpp)
    target_link_libraries(SummitServerAoiTests PRIVATE SummitServerBackend)
    summit_configure_target(SummitServerAoiTests)
    add_test(NAME SummitServer.Aoi COMMAND SummitServerAoiTests)
    set_tests_properties(SummitServer.Aoi PROPERTIES TIMEOUT 60)

    add_executable(SummitServerConsoleTests tests/ServerConsoleTest.cpp ServerConsole.cpp ServerConsole.h)
    target_include_directories(SummitServerConsoleTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(SummitServerConsoleTests PRIVATE ServerCore::ServerCore)
    summit_configure_target(SummitServerConsoleTests)
    add_test(NAME SummitServerConsoleInput COMMAND SummitServerConsoleTests)
    set_tests_properties(SummitServerConsoleInput PROPERTIES TIMEOUT 20)

    find_program(SUMMITSERVER_TEST_POWERSHELL NAMES powershell.exe powershell pwsh.exe pwsh)
    if (SUMMITSERVER_TEST_POWERSHELL)
        add_test(NAME SummitServerLoopback
                COMMAND "${SUMMITSERVER_TEST_POWERSHELL}"
                        -NoLogo
                        -NoProfile
                        -NonInteractive
                        -ExecutionPolicy Bypass
                        -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/LoopbackIntegration.ps1"
                        -ServerPath "$<TARGET_FILE:SummitServer>")
        set_tests_properties(SummitServerLoopback PROPERTIES
                TIMEOUT 20
                RESOURCE_LOCK SummitServerTcpPorts)
        add_test(NAME SummitServerOptions
                COMMAND "${SUMMITSERVER_TEST_POWERSHELL}"
                        -NoLogo
                        -NoProfile
                        -NonInteractive
                        -ExecutionPolicy Bypass
                        -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/ServerOptionsIntegration.ps1"
                        -ServerPath "$<TARGET_FILE:SummitServer>")
        set_tests_properties(SummitServerOptions PROPERTIES
                TIMEOUT 120
                RESOURCE_LOCK SummitServerTcpPorts)
        add_test(NAME SummitServerAnnouncements
                COMMAND "${SUMMITSERVER_TEST_POWERSHELL}"
                        -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass
                        -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/ConsoleAnnouncementIntegration.ps1"
                        -ServerPath "$<TARGET_FILE:SummitServer>")
        set_tests_properties(SummitServerAnnouncements PROPERTIES
                TIMEOUT 30
                RESOURCE_LOCK SummitServerTcpPorts)
        add_test(NAME SummitServer.Udp
                COMMAND "${SUMMITSERVER_TEST_POWERSHELL}"
                        -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass
                        -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/UdpIntegration.ps1"
                        -ServerPath "$<TARGET_FILE:SummitServer>")
        set_tests_properties(SummitServer.Udp PROPERTIES
                TIMEOUT 45
                RESOURCE_LOCK SummitServerTcpPorts)
    else ()
        message(WARNING "PowerShell was not found; SummitServerLoopback will not be registered")
    endif ()
endif ()
