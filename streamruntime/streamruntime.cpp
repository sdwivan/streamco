// streamruntime — a minimal OpenXR runtime scaffold for streamco.
//
// This is the proof-of-life layer described in DESIGN.md milestone 1:
// the loader can negotiate with us, the app can create an instance,
// query a stereo HMD system, enumerate view configurations, and request
// D3D11 graphics requirements. xrCreateSession is intentionally a stub
// that returns FUNCTION_UNSUPPORTED until milestone 2.

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Platform headers must precede <openxr/openxr_platform.h> — the OpenXR
// platform header references LUID / IUnknown / ID3D11Device /
// D3D_FEATURE_LEVEL / LARGE_INTEGER but does not include them itself.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <unknwn.h>
#include <d3d11.h>
#endif

#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_reflection.h>

#if defined(_WIN32)
#define RUNTIME_EXPORT __declspec(dllexport)
#else
#define RUNTIME_EXPORT __attribute__((visibility("default")))
#endif

namespace {

// ---------------------------------------------------------------------------
// Trivial logger. Replace with something real once you have more than one
// caller yelling into stderr.
// ---------------------------------------------------------------------------
#define SR_LOG(fmt, ...)                                                       \
    do {                                                                       \
        std::fprintf(stderr, "[streamruntime] " fmt "\n", ##__VA_ARGS__);      \
    } while (0)

// ---------------------------------------------------------------------------
// Handles. We box each handle as a pointer to an owned struct; the loader
// never inspects the bytes, so any non-null pointer is a valid handle value.
// ---------------------------------------------------------------------------
struct InstanceImpl {
    XrVersion apiVersion{};
    std::vector<std::string> enabledExtensions;
    std::atomic<uint64_t> nextPathId{1};
    std::mutex pathMutex;
    std::unordered_map<std::string, XrPath> pathByString;
    std::unordered_map<XrPath, std::string> stringByPath;
};

constexpr XrSystemId kHmdSystemId = 1;

std::mutex g_instancesMutex;
std::vector<InstanceImpl*> g_instances;

InstanceImpl* fromHandle(XrInstance h) { return reinterpret_cast<InstanceImpl*>(h); }
XrInstance toHandle(InstanceImpl* i) { return reinterpret_cast<XrInstance>(i); }

// ---------------------------------------------------------------------------
// Extensions we advertise. Start with D3D11 because hello_xr's default
// Windows build uses it. Add more as milestones land.
// ---------------------------------------------------------------------------
const std::vector<XrExtensionProperties> kSupportedExtensions = [] {
    auto make = [](const char* name, uint32_t version) {
        XrExtensionProperties p{XR_TYPE_EXTENSION_PROPERTIES};
        std::strncpy(p.extensionName, name, XR_MAX_EXTENSION_NAME_SIZE - 1);
        p.extensionVersion = version;
        return p;
    };
    return std::vector<XrExtensionProperties>{
        make(XR_KHR_D3D11_ENABLE_EXTENSION_NAME, 9),
    };
}();

// ---------------------------------------------------------------------------
// Core entry points. These are wired through xrGetInstanceProcAddr below.
// Everything here is milestone-1-minimum; real semantics come later.
// ---------------------------------------------------------------------------

XRAPI_ATTR XrResult XRAPI_CALL
sr_xrEnumerateApiLayerProperties(uint32_t, uint32_t* countOutput, XrApiLayerProperties*) {
    if (countOutput) *countOutput = 0;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
sr_xrEnumerateInstanceExtensionProperties(const char* /*layerName*/,
                                          uint32_t capacityInput,
                                          uint32_t* countOutput,
                                          XrExtensionProperties* properties) {
    if (!countOutput) return XR_ERROR_VALIDATION_FAILURE;
    const uint32_t n = static_cast<uint32_t>(kSupportedExtensions.size());
    *countOutput = n;
    if (capacityInput == 0) return XR_SUCCESS;
    if (capacityInput < n) return XR_ERROR_SIZE_INSUFFICIENT;
    for (uint32_t i = 0; i < n; ++i) properties[i] = kSupportedExtensions[i];
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
sr_xrCreateInstance(const XrInstanceCreateInfo* createInfo, XrInstance* instance) {
    if (!createInfo || !instance) return XR_ERROR_VALIDATION_FAILURE;

    auto* impl = new InstanceImpl();
    impl->apiVersion = createInfo->applicationInfo.apiVersion;
    for (uint32_t i = 0; i < createInfo->enabledExtensionCount; ++i) {
        impl->enabledExtensions.emplace_back(createInfo->enabledExtensionNames[i]);
    }

    {
        std::lock_guard<std::mutex> lk(g_instancesMutex);
        g_instances.push_back(impl);
    }

    *instance = toHandle(impl);
    SR_LOG("xrCreateInstance ok, app='%s', exts=%u",
           createInfo->applicationInfo.applicationName,
           createInfo->enabledExtensionCount);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
sr_xrDestroyInstance(XrInstance instance) {
    auto* impl = fromHandle(instance);
    if (!impl) return XR_ERROR_HANDLE_INVALID;
    {
        std::lock_guard<std::mutex> lk(g_instancesMutex);
        g_instances.erase(std::remove(g_instances.begin(), g_instances.end(), impl),
                          g_instances.end());
    }
    delete impl;
    SR_LOG("xrDestroyInstance ok");
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
sr_xrGetInstanceProperties(XrInstance instance, XrInstanceProperties* props) {
    if (!fromHandle(instance) || !props) return XR_ERROR_HANDLE_INVALID;
    props->runtimeVersion = XR_MAKE_VERSION(0, 1, 0);
    std::strncpy(props->runtimeName, "streamco streamruntime",
                 XR_MAX_RUNTIME_NAME_SIZE - 1);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
sr_xrPollEvent(XrInstance, XrEventDataBuffer*) {
    return XR_EVENT_UNAVAILABLE;
}

XRAPI_ATTR XrResult XRAPI_CALL
sr_xrResultToString(XrInstance, XrResult value, char buffer[XR_MAX_RESULT_STRING_SIZE]) {
    std::snprintf(buffer, XR_MAX_RESULT_STRING_SIZE, "XR_RESULT_%d", value);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
sr_xrStructureTypeToString(XrInstance, XrStructureType value, char buffer[XR_MAX_STRUCTURE_NAME_SIZE]) {
    std::snprintf(buffer, XR_MAX_STRUCTURE_NAME_SIZE, "XR_TYPE_%u",
                  static_cast<unsigned>(value));
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
sr_xrGetSystem(XrInstance instance, const XrSystemGetInfo* getInfo, XrSystemId* systemId) {
    if (!fromHandle(instance) || !getInfo || !systemId) return XR_ERROR_HANDLE_INVALID;
    if (getInfo->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY) {
        return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
    }
    *systemId = kHmdSystemId;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
sr_xrGetSystemProperties(XrInstance, XrSystemId systemId, XrSystemProperties* props) {
    if (systemId != kHmdSystemId || !props) return XR_ERROR_SYSTEM_INVALID;
    props->systemId = kHmdSystemId;
    props->vendorId = 0x5343; // "SC"
    std::strncpy(props->systemName, "streamco Virtual HMD",
                 XR_MAX_SYSTEM_NAME_SIZE - 1);
    props->graphicsProperties.maxSwapchainImageWidth = 4096;
    props->graphicsProperties.maxSwapchainImageHeight = 4096;
    props->graphicsProperties.maxLayerCount = XR_MIN_COMPOSITION_LAYERS_SUPPORTED;
    props->trackingProperties.orientationTracking = XR_TRUE;
    props->trackingProperties.positionTracking = XR_TRUE;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
sr_xrEnumerateEnvironmentBlendModes(XrInstance, XrSystemId systemId,
                                    XrViewConfigurationType viewConfig,
                                    uint32_t capacityInput, uint32_t* countOutput,
                                    XrEnvironmentBlendMode* modes) {
    if (systemId != kHmdSystemId) return XR_ERROR_SYSTEM_INVALID;
    if (viewConfig != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }
    if (!countOutput) return XR_ERROR_VALIDATION_FAILURE;
    *countOutput = 1;
    if (capacityInput == 0) return XR_SUCCESS;
    if (capacityInput < 1) return XR_ERROR_SIZE_INSUFFICIENT;
    modes[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
sr_xrEnumerateViewConfigurations(XrInstance, XrSystemId systemId,
                                 uint32_t capacityInput, uint32_t* countOutput,
                                 XrViewConfigurationType* types) {
    if (systemId != kHmdSystemId) return XR_ERROR_SYSTEM_INVALID;
    if (!countOutput) return XR_ERROR_VALIDATION_FAILURE;
    *countOutput = 1;
    if (capacityInput == 0) return XR_SUCCESS;
    if (capacityInput < 1) return XR_ERROR_SIZE_INSUFFICIENT;
    types[0] = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
sr_xrGetViewConfigurationProperties(XrInstance, XrSystemId systemId,
                                    XrViewConfigurationType type,
                                    XrViewConfigurationProperties* props) {
    if (systemId != kHmdSystemId || !props) return XR_ERROR_SYSTEM_INVALID;
    if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }
    props->viewConfigurationType = type;
    props->fovMutable = XR_TRUE;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
sr_xrEnumerateViewConfigurationViews(XrInstance, XrSystemId systemId,
                                     XrViewConfigurationType type,
                                     uint32_t capacityInput, uint32_t* countOutput,
                                     XrViewConfigurationView* views) {
    if (systemId != kHmdSystemId) return XR_ERROR_SYSTEM_INVALID;
    if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }
    if (!countOutput) return XR_ERROR_VALIDATION_FAILURE;
    *countOutput = 2;
    if (capacityInput == 0) return XR_SUCCESS;
    if (capacityInput < 2) return XR_ERROR_SIZE_INSUFFICIENT;

    XrViewConfigurationView v{XR_TYPE_VIEW_CONFIGURATION_VIEW};
    v.recommendedImageRectWidth = 1920;
    v.maxImageRectWidth = 4096;
    v.recommendedImageRectHeight = 1920;
    v.maxImageRectHeight = 4096;
    v.recommendedSwapchainSampleCount = 1;
    v.maxSwapchainSampleCount = 4;
    views[0] = v;
    views[1] = v;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
sr_xrGetD3D11GraphicsRequirementsKHR(XrInstance, XrSystemId systemId,
                                     XrGraphicsRequirementsD3D11KHR* req) {
    if (systemId != kHmdSystemId || !req) return XR_ERROR_SYSTEM_INVALID;
    // Zeroed adapterLuid means "any"; minFeatureLevel 11_0.
    req->adapterLuid = LUID{};
    req->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Not-yet-implemented. These exist so xrGetInstanceProcAddr can hand something
// back for names the app will look up. Milestone 2 fills these in properly.
// ---------------------------------------------------------------------------
XRAPI_ATTR XrResult XRAPI_CALL sr_xrCreateSession(XrInstance, const XrSessionCreateInfo*, XrSession*) {
    SR_LOG("xrCreateSession called — not implemented yet (milestone 2)");
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

// ---------------------------------------------------------------------------
// xrGetInstanceProcAddr — the dispatch table. Keep this the only place where
// string-to-fn lookups live.
// ---------------------------------------------------------------------------
XRAPI_ATTR XrResult XRAPI_CALL
sr_xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    if (!name || !function) return XR_ERROR_VALIDATION_FAILURE;
    *function = nullptr;

#define SR_BIND(fname) \
    if (std::strcmp(name, #fname) == 0) { \
        *function = reinterpret_cast<PFN_xrVoidFunction>(sr_##fname); \
        return XR_SUCCESS; \
    }

    // Loader queries these before an instance exists.
    SR_BIND(xrEnumerateApiLayerProperties)
    SR_BIND(xrEnumerateInstanceExtensionProperties)
    SR_BIND(xrCreateInstance)

    // Post-instance.
    SR_BIND(xrDestroyInstance)
    SR_BIND(xrGetInstanceProperties)
    SR_BIND(xrGetInstanceProcAddr)
    SR_BIND(xrPollEvent)
    SR_BIND(xrResultToString)
    SR_BIND(xrStructureTypeToString)
    SR_BIND(xrGetSystem)
    SR_BIND(xrGetSystemProperties)
    SR_BIND(xrEnumerateEnvironmentBlendModes)
    SR_BIND(xrEnumerateViewConfigurations)
    SR_BIND(xrGetViewConfigurationProperties)
    SR_BIND(xrEnumerateViewConfigurationViews)
    SR_BIND(xrCreateSession)

    // D3D11 extension — only bound when the instance enabled it, but we hand it
    // back unconditionally. Loader will have already validated the extension
    // enablement. Full spec compliance gets strict in milestone 2.
    SR_BIND(xrGetD3D11GraphicsRequirementsKHR)

#undef SR_BIND

    (void)instance;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

} // namespace

// ---------------------------------------------------------------------------
// The single exported entry point. Everything else is reached through the
// getInstanceProcAddr pointer we hand back here.
// ---------------------------------------------------------------------------
extern "C" RUNTIME_EXPORT XRAPI_ATTR XrResult XRAPI_CALL
xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo* loaderInfo,
                                  XrNegotiateRuntimeRequest* runtimeRequest) {
    if (!loaderInfo || !runtimeRequest ||
        loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
        loaderInfo->structSize != sizeof(XrNegotiateLoaderInfo) ||
        runtimeRequest->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST ||
        runtimeRequest->structVersion != XR_RUNTIME_INFO_STRUCT_VERSION ||
        runtimeRequest->structSize != sizeof(XrNegotiateRuntimeRequest) ||
        loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_RUNTIME_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_RUNTIME_VERSION) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    runtimeRequest->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    runtimeRequest->runtimeApiVersion = XR_CURRENT_API_VERSION;
    runtimeRequest->getInstanceProcAddr = sr_xrGetInstanceProcAddr;

    SR_LOG("negotiated with loader, iface=%u api=0x%llx",
           XR_CURRENT_LOADER_RUNTIME_VERSION,
           static_cast<unsigned long long>(XR_CURRENT_API_VERSION));
    return XR_SUCCESS;
}
