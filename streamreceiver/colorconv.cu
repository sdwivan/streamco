// NV12 (BT.709 limited-range) → BGRA8 full-range CUDA kernel.
// Writes directly into a CUDA surface object backed by a D3D12 shared
// texture (via cuImportExternalMemory + cuExternalMemoryGetMappedMipmappedArray).

#include <cuda_runtime.h>
#include <cstdint>

__device__ __forceinline__ uint8_t sat(int v) {
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

__global__ void nv12ToBgraKernel(const uint8_t* __restrict__ yPlane, int yPitch,
                                 const uint8_t* __restrict__ uvPlane, int uvPitch,
                                 cudaSurfaceObject_t dst,
                                 int dstX, int dstY,
                                 int width, int height) {
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    int Y = yPlane[y * yPitch + x];
    int U = uvPlane[(y >> 1) * uvPitch + (x & ~1) + 0];
    int V = uvPlane[(y >> 1) * uvPitch + (x & ~1) + 1];

    // BT.709 limited-range → full-range conversion (Q8 fixed-point).
    int C = Y - 16;
    int D = U - 128;
    int E = V - 128;
    int r = (298 * C           + 459 * E + 128) >> 8;
    int g = (298 * C -  55 * D - 136 * E + 128) >> 8;
    int b = (298 * C + 541 * D           + 128) >> 8;

    uchar4 px = make_uchar4(sat(b), sat(g), sat(r), 255);
    surf2Dwrite(px, dst, (dstX + x) * (int)sizeof(uchar4), dstY + y);
}

extern "C" cudaError_t launchNv12ToBgra(
    const uint8_t* y, int yPitch,
    const uint8_t* uv, int uvPitch,
    unsigned long long dstSurf,
    int dstX, int dstY,
    int width, int height,
    cudaStream_t stream)
{
    if (width <= 0 || height <= 0) return cudaSuccess;
    dim3 block(16, 16);
    dim3 grid((width + block.x - 1) / block.x, (height + block.y - 1) / block.y);
    nv12ToBgraKernel<<<grid, block, 0, stream>>>(
        y, yPitch, uv, uvPitch,
        (cudaSurfaceObject_t)dstSurf,
        dstX, dstY, width, height);
    return cudaGetLastError();
}
