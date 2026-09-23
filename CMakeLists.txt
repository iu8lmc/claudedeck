cmake_minimum_required(VERSION 3.21)
project(ClaudeDeck VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

set(CLAUDEDECK_SOURCES
    src/main.cpp
    src/Wcwidth.h
    src/VtEmulator.h
    src/VtEmulator.cpp
    src/ConPtySession.h
    src/ConPtySession.cpp
    src/TerminalWidget.h
    src/TerminalWidget.cpp
    src/SessionTab.h
    src/SessionTab.cpp
    src/NewSessionDialog.h
    src/NewSessionDialog.cpp
    src/MainWindow.h
    src/MainWindow.cpp
)

qt_add_executable(claudedeck WIN32 ${CLAUDEDECK_SOURCES})
target_link_libraries(claudedeck PRIVATE Qt6::Widgets)
target_include_directories(claudedeck PRIVATE src)

if(WIN32)
    target_compile_definitions(claudedeck PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00 WINVER=0x0A00)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(claudedeck PRIVATE util)   # forkpty() for the POSIX fallback backend
endif()

if(MSVC)
    target_compile_options(claudedeck PRIVATE /utf-8 /W3)
else()
    target_compile_options(claudedeck PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Tests: cmake -DCLAUDEDECK_BUILD_TESTS=ON
#   vt_test          — VT emulator unit tests (all platforms, no GUI)
#   smoke_test       — headless pty + widget check, renders a PNG (Linux/macOS only)
#   interactive_test — keyboard path + alternate screen through a real shell (Linux/macOS only)
option(CLAUDEDECK_BUILD_TESTS "Build the tests" OFF)
if(CLAUDEDECK_BUILD_TESTS)
    enable_testing()

    qt_add_executable(vt_test tests/vt_test.cpp src/VtEmulator.cpp src/VtEmulator.h src/Wcwidth.h)
    target_link_libraries(vt_test PRIVATE Qt6::Core)
    target_include_directories(vt_test PRIVATE src)
    if(MSVC)
        target_compile_options(vt_test PRIVATE /utf-8)
    endif()
    add_test(NAME vt_test COMMAND vt_test)

    if(NOT WIN32)
        set(CLAUDEDECK_CORE_SOURCES
            src/VtEmulator.cpp src/ConPtySession.cpp src/TerminalWidget.cpp src/SessionTab.cpp
            src/VtEmulator.h src/ConPtySession.h src/TerminalWidget.h src/SessionTab.h src/Wcwidth.h)
        foreach(t smoke_test interactive_test)
            qt_add_executable(${t} tests/${t}.cpp ${CLAUDEDECK_CORE_SOURCES})
            target_link_libraries(${t} PRIVATE Qt6::Widgets)
            target_include_directories(${t} PRIVATE src)
            if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
                target_link_libraries(${t} PRIVATE util)
            endif()
        endforeach()
        add_test(NAME smoke_test COMMAND smoke_test ${CMAKE_BINARY_DIR}/smoke.png)
    endif()
endif()
