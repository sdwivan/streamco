set(SDK "${CMAKE_SOURCE_DIR}/Video_Codec_SDK_12.2.72")

# Pin CUDA to the project-local toolkit at ${CMAKE_SOURCE_DIR}/cuda so builds
# are reproducible across machines regardless of what's installed system-wide.
set(CUDAToolkit_ROOT "${CMAKE_SOURCE_DIR}/cuda" CACHE PATH "CUDA Toolkit root" FORCE)
set(CUDA_PATH        "${CMAKE_SOURCE_DIR}/cuda" CACHE PATH "CUDA Toolkit root" FORCE)
set(CMAKE_CUDA_COMPILER "${CUDAToolkit_ROOT}/bin/nvcc.exe" CACHE FILEPATH "CUDA compiler (nvcc)" FORCE)

find_package(CUDAToolkit REQUIRED)

# CUDA 13.x's nvcc allowlists MSVC 2019..2022; MSVC 2026 (VS 18) trips the
# host_config.h version check. Pass -allow-unsupported-compiler through to
# nvcc so the identification pass and the real build both succeed. Must be
# set *before* enable_language(CUDA) so the compiler-probe step picks it up.
set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -allow-unsupported-compiler"
    CACHE STRING "" FORCE)
enable_language(CUDA)

add_executable(streamreceiver
    main.cpp
    colorconv.cu
    ${SDK}/Samples/NvCodec/NvDecoder/NvDecoder.cpp
)
target_compile_features(streamreceiver PRIVATE cxx_std_20)
target_compile_definitions(streamreceiver PRIVATE
    UNICODE _UNICODE NOMINMAX
    _WIN32_WINNT=0x0A00
    WIN32_LEAN_AND_MEAN
)
set_target_properties(streamreceiver PROPERTIES
    CUDA_ARCHITECTURES "native"
    CUDA_SEPARABLE_COMPILATION ON
)
target_include_directories(streamreceiver PRIVATE
    ${SDK}/Interface
    ${SDK}/Samples/NvCodec
    ${SDK}/Samples/Utils
)
target_link_directories(streamreceiver PRIVATE ${SDK}/Lib/x64)
target_link_libraries(streamreceiver PRIVATE
    d3d12 dxgi dxguid
    nvcuvid
    CUDA::cuda_driver
    CUDA::cudart
    ws2_32
)
