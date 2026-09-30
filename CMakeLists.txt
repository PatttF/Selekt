cmake_minimum_required(VERSION 3.24)
project(Selekt C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

if(ANDROID)
    # ── Android App Glue ──────────────────────────────────────────────────────────
    add_library(native_app_glue STATIC
        ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
    target_include_directories(native_app_glue PUBLIC
        ${ANDROID_NDK}/sources/android/native_app_glue)
else()
    # ── GLFW ──────────────────────────────────────────────────────────────────────
    FetchContent_Declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG        3.4)
    set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(glfw)
endif()

# ── Dear ImGui ────────────────────────────────────────────────────────────────
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.8)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

if(ANDROID)
    add_library(imgui_lib STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_android.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
else()
    add_library(imgui_lib STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
endif()

target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)

if(ANDROID)
    target_link_libraries(imgui_lib PUBLIC EGL GLESv3 android log)
else()
    target_link_libraries(imgui_lib PUBLIC glfw)
    if(APPLE)
        target_link_libraries(imgui_lib PUBLIC
            "-framework OpenGL" "-framework Cocoa" "-framework IOKit" "-framework CoreVideo")
    elseif(WIN32)
        target_link_libraries(imgui_lib PUBLIC opengl32 gdi32 imm32)
    else()
        find_package(OpenGL REQUIRED)
        target_link_libraries(imgui_lib PUBLIC OpenGL::GL)
    endif()
endif()

# ── RtMidi ────────────────────────────────────────────────────────────────────
set(RTMIDI_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS    OFF CACHE BOOL "" FORCE)
set(RTMIDI_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)

if(ANDROID)
    set(RTMIDI_API_AMIDI ON CACHE BOOL "" FORCE)
endif()

FetchContent_Declare(rtmidi
    GIT_REPOSITORY https://github.com/thestk/rtmidi.git
    GIT_TAG        6.0.0)
FetchContent_GetProperties(rtmidi)
if(NOT rtmidi_POPULATED)
    FetchContent_Populate(rtmidi)
    add_subdirectory(${rtmidi_SOURCE_DIR} ${rtmidi_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()

# ── Executable ────────────────────────────────────────────────────────────────
if(ANDROID)
    add_library(selekt SHARED src/main.cpp)
elseif(APPLE)
    add_executable(selekt MACOSX_BUNDLE src/main.cpp)
    set_target_properties(selekt PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "SELEKT"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.selekt.app")
elseif(WIN32)
    add_executable(selekt WIN32 src/main.cpp)
else()
    add_executable(selekt src/main.cpp)
endif()

if(ANDROID)
    target_link_libraries(selekt PRIVATE imgui_lib)
else()
    target_link_libraries(selekt PRIVATE imgui_lib rtmidi)
endif()

if(ANDROID)
    target_link_libraries(selekt PRIVATE native_app_glue android log EGL GLESv3 amidi m jnigraphics )
    target_compile_definitions(selekt PRIVATE __ANDROID_API__=29)
elseif(APPLE)
    target_link_libraries(selekt PRIVATE
        "-framework CoreMIDI" "-framework CoreAudio" "-framework CoreFoundation"
        "-framework AudioToolbox" "-framework AudioUnit")
    target_compile_definitions(selekt PRIVATE __MACOSX_CORE__)
elseif(WIN32)
    target_link_libraries(selekt PRIVATE winmm ole32 comdlg32)
    target_compile_definitions(selekt PRIVATE __WINDOWS_MM__)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(selekt PRIVATE asound Threads::Threads dl m)
    target_compile_definitions(selekt PRIVATE __LINUX_ALSA__)
endif()

message(STATUS "Build: ${CMAKE_BUILD_TYPE}")
