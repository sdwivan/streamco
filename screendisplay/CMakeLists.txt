add_executable(screendisplay main.cpp)
target_compile_features(screendisplay PRIVATE cxx_std_20)
target_compile_definitions(screendisplay PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(screendisplay PRIVATE d3d12 dxgi dxguid d3dcompiler)
