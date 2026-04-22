add_executable(chromecapture main.cpp)
target_compile_features(chromecapture PRIVATE cxx_std_20)
target_compile_definitions(chromecapture PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(chromecapture PRIVATE
    d3d11 d3d12 dxgi dxguid
    windowsapp dwmapi
)
