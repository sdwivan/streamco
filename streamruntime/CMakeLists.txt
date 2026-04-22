add_library(streamruntime MODULE streamruntime.cpp)

target_link_libraries(streamruntime PRIVATE OpenXR::headers)

target_compile_features(streamruntime PRIVATE cxx_std_20)

if(WIN32)
    target_sources(streamruntime PRIVATE streamruntime.def)
    target_compile_definitions(streamruntime PRIVATE
        XR_USE_PLATFORM_WIN32
        XR_USE_GRAPHICS_API_D3D11
    )
    target_link_libraries(streamruntime PRIVATE d3d11)
endif()

set_target_properties(streamruntime PROPERTIES
    OUTPUT_NAME streamruntime
    PREFIX ""
)

# Emit the OpenXR runtime manifest JSON next to the DLL. The loader reads this
# via the HKLM\Software\Khronos\OpenXR\1\ActiveRuntime registry value (Windows)
# or $XDG_CONFIG_DIRS/openxr/1/active_runtime.json (Linux).
set(_streamruntime_manifest "${CMAKE_CURRENT_BINARY_DIR}/streamruntime.json")
file(GENERATE
    OUTPUT "${_streamruntime_manifest}"
    CONTENT
"{
    \"file_format_version\": \"1.0.0\",
    \"runtime\": {
        \"api_version\": \"1.0\",
        \"library_path\": \"$<TARGET_FILE_NAME:streamruntime>\",
        \"name\": \"streamco streamruntime\"
    }
}
"
)

install(TARGETS streamruntime
    RUNTIME DESTINATION bin/openxr
    LIBRARY DESTINATION bin/openxr)
install(FILES "${_streamruntime_manifest}"
    DESTINATION bin/openxr)
