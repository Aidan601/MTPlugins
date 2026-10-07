# CMake alternative to the .vcxproj: cmake -S . -B out -A x64, then cmake --build out --config Release.
# The DLL is copied to <game>\bin\plugins\MapEditExample\ (game folder from TA_TOOLS_PATH or -DBO3_ROOT=<folder>), -DDEPLOY_TO_GAME=OFF skips it.
cmake_minimum_required(VERSION 3.15)
project(MapEditExample CXX)

# the plugin's name: the DLL name, its folder under bin\plugins, and ModToolsPluginApi::name in the .cpp must all match
set(PLUGIN_NAME MapEditExample)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Radiant and APE are 64-bit: configure with -A x64")
endif()

find_path(MODTOOLS_SDK_DIR modtools_plugin.h
    PATHS "${CMAKE_CURRENT_LIST_DIR}/.." "${CMAKE_CURRENT_LIST_DIR}/../.." NO_DEFAULT_PATH)
if(NOT MODTOOLS_SDK_DIR)
    message(FATAL_ERROR "modtools_plugin.h not found: keep this project inside the SDK folder or pass -DMODTOOLS_SDK_DIR=<SDK folder>")
endif()

set(BO3_ROOT "$ENV{TA_TOOLS_PATH}" CACHE PATH "Black Ops III folder (contains bin\\Radiant_modtools.exe)")
option(DEPLOY_TO_GAME "Copy the DLL to <BO3_ROOT>\\bin\\plugins\\<name> after a build" ON)

# static runtime (/MT): the DLL needs no Visual C++ redistributable on the user's PC
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(${PLUGIN_NAME} SHARED MapEditExample.cpp)
set_target_properties(${PLUGIN_NAME} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON OUTPUT_NAME ${PLUGIN_NAME})
target_include_directories(${PLUGIN_NAME} PRIVATE "${MODTOOLS_SDK_DIR}")
target_compile_definitions(${PLUGIN_NAME} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(${PLUGIN_NAME} PRIVATE user32)

string(STRIP "${BO3_ROOT}" BO3_ROOT_CLEAN)
if(DEPLOY_TO_GAME AND BO3_ROOT_CLEAN)
    set(DEPLOY_DIR "${BO3_ROOT_CLEAN}/bin/plugins/${PLUGIN_NAME}")
    add_custom_command(TARGET ${PLUGIN_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DEPLOY_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${PLUGIN_NAME}>" "${DEPLOY_DIR}"
        COMMENT "Copying ${PLUGIN_NAME}.dll to ${DEPLOY_DIR} (if this fails, untick the plugin in the Plugins window first)")
elseif(DEPLOY_TO_GAME)
    message(WARNING "TA_TOOLS_PATH is not set: pass -DBO3_ROOT=<Black Ops III folder> to copy the DLL to the game after a build")
endif()
