
cmake_minimum_required(VERSION 3.24)
project(PD2VRChatBuffer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

include(FetchContent)

# Official SuperBLT native-plugin template SDK.
FetchContent_Declare(
    sblt_template
    GIT_REPOSITORY https://github.com/diesel-modding/PAYDAY2-SuperBLT-DLL-Template.git
    GIT_TAG master
)
FetchContent_Populate(sblt_template)

# Official Valve OpenVR SDK.
FetchContent_Declare(
    openvr
    GIT_REPOSITORY https://github.com/ValveSoftware/openvr.git
    GIT_TAG master
)
FetchContent_Populate(openvr)

add_library(pd2_vr_chat_buffer SHARED
    src/main.cpp
    ${sblt_template_SOURCE_DIR}/sdk/setup.cpp
    ${sblt_template_SOURCE_DIR}/sdk/plugin_sdk_import.gen.cpp
)

target_compile_definitions(pd2_vr_chat_buffer PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(pd2_vr_chat_buffer PRIVATE
    ${sblt_template_SOURCE_DIR}/sdk
    ${openvr_SOURCE_DIR}/headers
)

# OpenVR's prebuilt 64-bit import library from the official SDK.
target_link_libraries(pd2_vr_chat_buffer PRIVATE
    "${openvr_SOURCE_DIR}/lib/win64/openvr_api.lib"
)

set_target_properties(pd2_vr_chat_buffer PROPERTIES
    OUTPUT_NAME "pd2_vr_chat_buffer"
)
