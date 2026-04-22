add_executable(screencapture main.cpp)
target_compile_features(screencapture PRIVATE cxx_std_20)
target_compile_definitions(screencapture PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(screencapture PRIVATE d3d11 d3d12 dxgi dxguid)
