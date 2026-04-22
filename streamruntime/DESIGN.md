# streamruntime — design

An OpenXR runtime that presents as a head-mounted display, accepts the
application's rendered frames, and feeds them into streamco's existing
capture/encode/stream pipeline. This document describes how the pieces fit
together and the order in which to build them.

## Goals

1. Be a conformant-enough OpenXR 1.0+ runtime that commercial OpenXR apps
   (hello_xr, Unity/Unreal/OpenXR Toolkit demos, simple native samples) can
   initialize and submit frames without crashing.
2. Hand off each submitted frame to streamco with minimal latency — no
   extra GPU copies if we can avoid it.
3. Accept input (pose, controllers) either from a local simulation or from
   a remote viewer over the network — same wire protocol streamco already
   uses for video.
4. Be Windows-first, D3D11-first. Add D3D12, Vulkan, and Linux after the
   Windows D3D11 path runs end-to-end.

## Non-goals

- OpenXR conformance pass. Pass the subset of the CTS we need, no more.
- Supporting every interaction profile. Start with `khr/simple_controller`.
- Supporting every extension. Start with `XR_KHR_D3D11_enable`.
- Multi-app / multi-session concurrency. One app at a time.
- Being a compositor for third-party content on a real HMD. This is a
  *virtual* HMD — we composite into a texture, not a display scanout.

## System diagram

```
+------------------+   loader   +----------------+   shared   +------------+
|  OpenXR app      | ---------> | OpenXR loader  | ---------> | stream-    |
|  (hello_xr,      |   (DLL)    | openxr_loader. |            | runtime.   |
|   Unity, etc.)   | <--------- | dll            | <--------- | dll        |
+------------------+            +----------------+            +-----+------+
                                                                    |
                                    +------------------+            |
                                    |  streamsender /  | <----------+
                                    |  chromecapture   |  texture/share
                                    +---------+--------+
                                              |
                                              v
                                        network / file
                                              |
                                              v
                                    +------------------+
                                    |  streamreceiver  |
                                    |  screendisplay   |
                                    +------------------+
```

App -> loader -> streamruntime is standard OpenXR dispatch. streamruntime ->
streamsender is an in-process handoff (same DLL, shared D3D11 texture) in v1,
likely an IPC shared-handle bridge in v2 so streamsender can be its own
process.

## Process / threading model

- **App thread** calls into us through the loader. All `xr*` entry points
  run on whichever thread the app picked. Assume any thread.
- **Frame submission** (`xrEndFrame`) is the only hot path. It must be
  non-blocking from the app's perspective — composite inline, enqueue for
  stream, return.
- **Streaming thread** (owned by streamsender) pulls the most recent
  composited frame, encodes with NVENC, sends. Dropping frames here is
  better than stalling the app.
- **Pose thread** reads remote pose updates from the network, writes into
  a double-buffered pose slot read by `xrLocateViews` / `xrWaitFrame`.

Invariant: the app thread never blocks on the network. The streaming
thread never blocks on the app.

## Frame lifecycle

OpenXR's frame loop has three calls the runtime owns:

| Call | Runtime responsibility |
|------|------------------------|
| `xrWaitFrame` | Produce a predicted display time. Throttle the app to the target FPS (start: 72 Hz matching Quest 2). Return the current pose prediction. |
| `xrBeginFrame` | Mark the frame as started. Accept `shouldRender = true` unless we're deliberately skipping. |
| `xrEndFrame` | Receive composition layers (projection + quads). Composite into the shared texture. Signal the streamer. |

Timing: `xrWaitFrame` blocks until the next frame budget opens. Simplest
implementation is a steady clock + sleep; better later is to sync to the
remote viewer's vsync signal carried over the stream protocol.

## Compositing

**Milestone 2** (first working path): single projection layer, two views.
App submits a left-eye image and a right-eye image via `XrCompositionLayerProjection`.
We blit both into a single side-by-side texture.

```
 shared target texture (2W x H)
+-----------------+-----------------+
|   left-eye      |   right-eye     |
|   swapchain     |   swapchain     |
|   image         |   image         |
+-----------------+-----------------+
```

Implementation is a D3D11 `CopySubresourceRegion` per eye into the shared
target. No shaders, no resampling in v1.

**Milestone 3**: multiple projection layers + quad layers. Requires a
real compositor pass — fullscreen triangle + shader that samples layers
back-to-front with per-layer blend state and alpha. Depth-test off; the
app's depth is not available to us unless `XR_KHR_composition_layer_depth`
is enabled (defer).

**Milestone 4**: distortion. Real HMDs apply lens distortion here. For
streaming we have a choice: apply distortion (if the viewer is a real
HMD being fed our frames) or don't (if the viewer is a flat display). Make
this a configurable post-pass, default off.

## Swapchain management

Swapchains are the runtime-owned ring of textures the app renders into.
Minimum viable implementation:

- Three images per swapchain (standard triple buffering).
- `xrCreateSwapchain` creates D3D11 2D textures with the format/size the
  app asked for. Bind flags: `D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET`.
- `xrAcquireSwapchainImage` hands back the next index in round-robin.
- `xrWaitSwapchainImage` — short-term, return immediately. Long-term, wait
  on a D3D11 query or keyed mutex if we ever move to cross-process.
- `xrReleaseSwapchainImage` — mark the image as "submittable." `xrEndFrame`
  is the point where we actually sample it.

Format support in v1: `DXGI_FORMAT_R8G8B8A8_UNORM_SRGB` and
`DXGI_FORMAT_B8G8R8A8_UNORM_SRGB`. Others return `XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED`.

## Spaces and poses

OpenXR's space model: `XrSpace` is a frame of reference. `xrLocateViews`
returns each eye's pose relative to a space. `xrLocateSpace` returns one
space relative to another.

Simplest v1 implementation: treat `VIEW`, `LOCAL`, and `STAGE` as
identity-parented with no offsets. Eye poses are a fixed IPD-apart pair
that moves with head pose. Head pose comes from:

- **Local mode**: keyboard/mouse, for dev testing. WASD + mouse-look.
- **Remote mode**: decoded from streamreceiver protocol. Latest pose wins.
- **Replay mode**: read from a file, for deterministic testing.

Pose is 6-DOF (position + orientation as quaternion). Pick coordinate
convention early: OpenXR uses right-handed, +Y up, -Z forward. Whatever
streamco is already using for camera capture may differ — document and
convert at the boundary.

## Actions and input

This is the most painful part of the OpenXR API. For v1:

- Stub `xrCreateActionSet`, `xrCreateAction`, `xrSuggestInteractionProfileBindings`,
  `xrAttachSessionActionSets` — store the bindings, don't interpret them.
- `xrSyncActions` — always succeeds, produces no state changes.
- `xrGetActionStateBoolean/Float/Vector2f/Pose` — return inactive /
  identity / neutral.
- Interaction profile suggested: `/interaction_profiles/khr/simple_controller`.

This gets most apps past initialization even if controllers don't work.
Real input mapping is milestone 5.

## Extensions

v1 extension list (advertise, and implement the parts exercised by
hello_xr):

- `XR_KHR_D3D11_enable` — required for Windows D3D11 apps.
- `XR_EXT_debug_utils` — cheap to implement (logger), saves pain later.

Deferred:

- `XR_KHR_D3D12_enable`, `XR_KHR_vulkan_enable2` — milestone 6+.
- `XR_KHR_composition_layer_depth` — useful for async reprojection if we
  ever add it. Not needed for streaming.
- `XR_EXT_hand_tracking` — nice-to-have, not critical.

## Streaming handoff

The composited frame needs to reach `streamsender` with as few copies as
possible. Two designs:

1. **In-process (v1)**: streamruntime and streamsender live in the same
   DLL or call each other directly. The shared target texture is a plain
   `ID3D11Texture2D` the streamer samples on its own thread. Needs a
   keyed-mutex or a fence + ring of target textures to avoid tearing.

2. **Cross-process (v2)**: open the target texture as a
   `D3D11_RESOURCE_MISC_SHARED_NTHANDLE`, send the NT handle to
   streamsender.exe over a named pipe, streamsender reopens it. Higher
   latency, better isolation, streamsender can crash independently.

Start with option 1. Cut to option 2 only if the combined process is
unstable or if streamsender needs privileges streamruntime shouldn't have.

## Discovery / registration

On Windows, the loader finds the active runtime through:

```
HKEY_LOCAL_MACHINE\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime = "C:\...\streamruntime.json"
```

`streamruntime.json` is generated next to the DLL by our CMake build. To
point the loader at our build during development:

```powershell
# Run as admin, from the streamco build directory:
reg add "HKLM\SOFTWARE\Khronos\OpenXR\1" ^
  /v ActiveRuntime ^
  /t REG_SZ ^
  /d "%CD%\streamruntime\streamruntime.json" ^
  /f
```

Snapshot the pre-existing value first so you can restore it:

```powershell
reg query "HKLM\SOFTWARE\Khronos\OpenXR\1" /v ActiveRuntime > prior_runtime.txt
```

Uninstall tool later: a small script that restores the saved path.

## Milestones

### M1 — loader negotiation proof of life
- [x] DLL builds
- [x] `xrNegotiateLoaderRuntimeInterface` succeeds
- [x] `xrCreateInstance` + `xrGetSystem` + view configuration queries return sensible values
- [ ] `hello_xr` reaches `xrCreateSession` and prints its "unsupported" error from us
- [ ] Runtime DLL loads in a debugger, we see our log lines

**Exit criteria**: `hello_xr -g D3D11` launches with our runtime active,
logs show our DLL was loaded and negotiated, app cleanly reports it can't
get past session create (because we return `FUNCTION_UNSUPPORTED`).

### M2 — session + swapchain + single projection layer
- [ ] `xrCreateSession` with D3D11 binding
- [ ] Session state machine (`IDLE → READY → SYNCHRONIZED → VISIBLE → FOCUSED`) via events
- [ ] Swapchain create / acquire / wait / release
- [ ] Frame loop (`xrWaitFrame`, `xrBeginFrame`, `xrEndFrame`) pinned at 72 Hz
- [ ] Composite single projection layer into side-by-side target
- [ ] Dump target texture to PNG per N frames for manual verification

**Exit criteria**: `hello_xr` runs to completion rendering into our
runtime. PNG dumps show the left and right eye content.

### M3 — streamsender integration
- [ ] Shared D3D11 target texture handed to streamsender
- [ ] Streamsender encodes target and ships frames
- [ ] streamreceiver on another machine displays the result
- [ ] End-to-end latency measurement (app render → remote display)

**Exit criteria**: `hello_xr` rendering on machine A, visible on the
screen of machine B through streamreceiver, latency < target.

### M4 — pose input from remote
- [ ] Pose channel in the streamco wire protocol (probably already exists
      for camera control; reuse it)
- [ ] `xrLocateViews` returns the remotely-supplied pose
- [ ] Latency compensation: prediction based on app-reported display time

**Exit criteria**: moving at the remote viewer changes the rendered view
on the app host.

### M5 — actions / controllers
- [ ] Real `xrSyncActions` that reads controller state from the remote
      protocol
- [ ] `khr/simple_controller` interaction profile
- [ ] Trigger / grip / pose on both hands

### M6+ — graphics APIs, multiple layers, conformance
- [ ] D3D12 support
- [ ] Vulkan support (KHR_vulkan_enable2)
- [ ] Quad layers
- [ ] Subset of CTS passing

## Open questions

1. **Who owns streamsender?** If streamruntime loads streamsender in-process,
   we inherit its dependencies into every OpenXR app. That's a big deal —
   NVENC / CUDA DLL dependencies loaded inside Unity is a support nightmare.
   Favor keeping streamsender a separate process with a shared-handle bridge
   once M3 proves the pipeline.

2. **What's the target latency budget?** This drives most of the compositor
   design. If < 20 ms end-to-end, we need zero-copy + fence-based wait. If
   50 ms is fine, the simple path is fine.

3. **Do we want to support VR compositor behavior** (async reprojection,
   app drop-frame handling)? Not needed for "virtual HMD → streaming,"
   but if we ever want to stream to a real HMD headset, yes. Defer.

4. **Runtime-side vs app-side warp**. Applying lens distortion in the runtime
   means the wire protocol carries already-distorted frames and the remote
   decoder just displays them. Doing it on the decoder side means the
   protocol carries clean projection frames and gives the decoder freedom
   to target any lens. Decoder-side is more flexible but costs GPU on the
   viewer. Decision deferred to M3+.

5. **Coordinate system convention.** OpenXR: RH, +Y up, -Z forward. Confirm
   what streamco's existing camera code uses and where the conversion
   happens.

## References

- Spec: https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html
- Loader negotiation header: `OpenXR-SDK-Source/src/loader/openxr_loader_negotiation.h`
- Reference minimal runtime: `OpenXR-SDK-Source/src/tests/test_runtimes/runtime_test.cpp`
- Reference open runtime (full, complex): https://gitlab.freedesktop.org/monado/monado
- Closest-to-streamco prior art: ALVR — https://github.com/alvr-org/ALVR
