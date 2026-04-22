cmake_minimum_required(VERSION 4.2)
project(streamco)

set(CMAKE_CXX_STANDARD 20)

# --- OpenXR SDK (vendored) -------------------------------------------------
# Build the loader from OpenXR-SDK-Source as part of this tree. Tests,
# api-layers and conformance are off by default for this project.
set(OPENXR_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/OpenXR-SDK-Source")
set(BUILD_TESTS ON CACHE BOOL "OpenXR: build tests (includes hello_xr sample)" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "OpenXR: build API layers" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "OpenXR: build conformance tests" FORCE)
set(BUILD_LOADER ON CACHE BOOL "OpenXR: build loader" FORCE)
add_subdirectory(${OPENXR_SDK_DIR} EXCLUDE_FROM_ALL)
# ---------------------------------------------------------------------------

add_executable(streamco main.cpp)
target_link_libraries(streamco PRIVATE OpenXR::openxr_loader)

add_subdirectory(screencapture)
add_subdirectory(screendisplay)
add_subdirectory(chromecapture)
add_subdirectory(streamsender)
add_subdirectory(streamreceiver)
add_subdirectory(streamruntime)

# --- Install ---------------------------------------------------------------
install(TARGETS streamco RUNTIME DESTINATION bin)

# Install the OpenXR loader alongside streamco so the runtime can find it.
if(TARGET openxr_loader)
    install(
        TARGETS openxr_loader
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
    )
    install(
        DIRECTORY "${OPENXR_SDK_DIR}/include/openxr/"
        DESTINATION include/openxr
        FILES_MATCHING PATTERN "*.h"
    )
endif()
