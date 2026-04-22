set(SDK "${CMAKE_SOURCE_DIR}/Video_Codec_SDK_12.2.72")

add_executable(streamsender
    main.cpp
    ${SDK}/Samples/NvCodec/NvEncoder/NvEncoder.cpp
    ${SDK}/Samples/NvCodec/NvEncoder/NvEncoderD3D12.cpp
)
target_compile_features(streamsender PRIVATE cxx_std_20)
target_compile_definitions(streamsender PRIVATE
    UNICODE _UNICODE NOMINMAX
    _WIN32_WINNT=0x0A00
    WIN32_LEAN_AND_MEAN
)
target_include_directories(streamsender PRIVATE
    ${SDK}/Interface
    ${SDK}/Samples/NvCodec
    ${SDK}/Samples/Utils
)
target_link_directories(streamsender PRIVATE ${SDK}/Lib/x64)
target_link_libraries(streamsender PRIVATE
    d3d12 dxgi dxguid
    nvencodeapi
    ws2_32
)
