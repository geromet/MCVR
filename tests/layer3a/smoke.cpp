// MCVR Layer-3A software-Vulkan verification smoke (#501).
//
// This is a test-only executable. It is built twice:
//
//   layer3a_smoke            - compiled with MCVR_LAYER3A_TEST_CPU_ADMISSION and
//                              therefore allowed to use Admission::TestAllowCpuDevice;
//   layer3a_production_policy - compiled without it, so --allow-cpu is refused and
//                              the production discrete-only policy is exercised.
//
// Both targets share src/core/vulkan/device_policy.hpp with the renderer, so the
// admitted capability contract is the production one.
//
// Modes:
//   --preflight        instance (no surface), enumerate devices, report capabilities
//   --smoke            GLFW/X11 window, surface, device, queues, swapchain create/destroy
//   --canary           require a deterministic validation error (error channel proof)
//   --prod-reject      require the production policy to reject a CPU-only enumeration
//                      (exit 3 = rejected as expected, 4 = unexpectedly admitted)
//
// Exit codes: 0 success, 1 unexpected failure, 3/4 as described for --prod-reject.

#if defined(__linux__) || defined(__unix__)
#    ifndef VK_USE_PLATFORM_XLIB_KHR
#        define VK_USE_PLATFORM_XLIB_KHR 1
#    endif
#    ifndef VK_USE_PLATFORM_XCB_KHR
#        define VK_USE_PLATFORM_XCB_KHR 1
#    endif
#endif

#include "core/vulkan/device_policy.hpp"
#include <vulkan/vulkan.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using mcvr::device_policy::Admission;
using mcvr::device_policy::CapabilityMask;
using mcvr::device_policy::CapabilityReport;

constexpr uint32_t kWindowWidth = 256;
constexpr uint32_t kWindowHeight = 256;

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

std::string jstr(const std::string &value) {
    std::string out = "\"";
    for (unsigned char c : value) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += "\"";
    return out;
}


std::string ju32(uint32_t value) { return std::to_string(value); }
std::string jbool(bool value) { return value ? "true" : "false"; }

struct Evidence {
    std::string mode;
    std::string terminalPhase = "none";
    bool ok = false;
    std::vector<std::pair<std::string, std::string>> fields;

    void add(const std::string &key, const std::string &encodedValue) {
        fields.emplace_back(key, encodedValue);
    }
    void addStr(const std::string &key, const std::string &value) { add(key, jstr(value)); }
    void addU32(const std::string &key, uint32_t value) { add(key, ju32(value)); }
    void addBool(const std::string &key, bool value) { add(key, jbool(value)); }

    std::string json() const {
        std::ostringstream out;
        out << "{";
        out << "\"mode\":" << jstr(mode) << ",\"ok\":" << jbool(ok)
            << ",\"terminal_phase\":" << jstr(terminalPhase);
        for (const auto &[key, value] : fields) { out << "," << jstr(key) << ":" << value; }
        out << "}";
        return out.str();
    }
};

std::string gEmitPath;

void emit(const Evidence &evidence) {
    const std::string json = evidence.json();
    std::cout << json << std::endl;
    if (!gEmitPath.empty()) {
        std::ofstream file(gEmitPath, std::ios::trunc);
        if (!file) { std::cerr << "cannot write emit path " << gEmitPath << std::endl; }
        file << json << "\n";
    }
}

[[noreturn]] void failWith(const char *phase, const std::string &message) {
    Evidence evidence;
    evidence.ok = false;
    evidence.terminalPhase = phase;
    evidence.addStr("error", message);
    emit(evidence);
    std::exit(1);
}

std::string vkResultName(VkResult result) {
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_EVENT_SET: return "VK_EVENT_SET";
    case VK_EVENT_RESET: return "VK_EVENT_RESET";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    default: return "VK_RESULT_" + std::to_string(static_cast<int>(result));
    }
}

void requireVk(VkResult result, const char *phase, const char *call) {
    if (result != VK_SUCCESS) {
        failWith(phase, std::string(call) + " returned " + vkResultName(result));
    }
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

struct ValidationState {
    bool required = false;
    bool enabled = false;
    uint32_t errors = 0;
    uint32_t warnings = 0;
    std::string layerSpecVersion = "none";
    std::string firstError;
};

ValidationState *gValidation = nullptr;

VKAPI_ATTR VkBool32 VKAPI_CALL validationCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                  VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                                                  const VkDebugUtilsMessengerCallbackDataEXT *data,
                                                  void * /*userData*/) {
    if (gValidation == nullptr) { return VK_FALSE; }
    const char *id = (data != nullptr && data->pMessageIdName != nullptr) ? data->pMessageIdName : "unknown";
    const char *message = (data != nullptr && data->pMessage != nullptr) ? data->pMessage : "";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        gValidation->errors += 1;
        if (gValidation->firstError.empty()) { gValidation->firstError = std::string(id) + ": " + message; }
        std::cerr << "VALIDATION_ERROR | id=" << id << " | " << message << std::endl;
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        gValidation->warnings += 1;
        std::cerr << "VALIDATION_WARNING | id=" << id << " | " << message << std::endl;
    }
    return VK_FALSE;
}

bool hasInstanceLayer(const char *name) {
    uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) { return false; }
    std::vector<VkLayerProperties> layers(count);
    if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) { return false; }
    for (const auto &layer : layers) {
        if (std::strcmp(layer.layerName, name) == 0) { return true; }
    }
    return false;
}

bool hasInstanceExtension(const char *name) {
    uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS) { return false; }
    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()) != VK_SUCCESS) { return false; }
    for (const auto &extension : extensions) {
        if (std::strcmp(extension.extensionName, name) == 0) { return true; }
    }
    return false;
}

struct InstanceResources {
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
};

InstanceResources createInstance(bool requireValidation, bool withSurface, Evidence &evidence) {
    InstanceResources resources;

    uint32_t loaderVersion = 0;
    VkResult versionResult = vkEnumerateInstanceVersion(&loaderVersion);
    evidence.addStr("loader_version_result", vkResultName(versionResult));
    if (versionResult != VK_SUCCESS) { loaderVersion = VK_API_VERSION_1_0; }
    evidence.addStr("loader_api_version",
                    std::to_string(VK_VERSION_MAJOR(loaderVersion)) + "." +
                        std::to_string(VK_VERSION_MINOR(loaderVersion)) + "." +
                        std::to_string(VK_VERSION_PATCH(loaderVersion)));

    const bool validationAvailable = hasInstanceLayer("VK_LAYER_KHRONOS_validation");
    evidence.addBool("validation_layer_available", validationAvailable);
    if (requireValidation && !validationAvailable) {
        failWith("validation_layer_missing",
                 "VK_LAYER_KHRONOS_validation is not available; proof-bearing runs require it");
    }
    if (requireValidation && !hasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        failWith("validation_error_channel_missing", "VK_EXT_debug_utils is unavailable");
    }

    std::vector<const char *> extensions;
    if (withSurface) {
        extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        // GLFW may be compiled to use either the Xlib or the XCB Vulkan surface
        // path. Enable whichever the loader exposes; at least one is required.
        bool surfacePath = false;
        if (hasInstanceExtension(VK_KHR_XLIB_SURFACE_EXTENSION_NAME)) {
            extensions.push_back(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
            surfacePath = true;
        }
        if (hasInstanceExtension(VK_KHR_XCB_SURFACE_EXTENSION_NAME)) {
            extensions.push_back(VK_KHR_XCB_SURFACE_EXTENSION_NAME);
            surfacePath = true;
        }
        if (!surfacePath) {
            failWith("surface_extension_missing", "neither VK_KHR_xlib_surface nor VK_KHR_xcb_surface is available");
        }
    }
    const bool debugUtils = requireValidation || hasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (debugUtils) { extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME); }

    std::vector<const char *> layers;
    if (requireValidation) { layers.push_back("VK_LAYER_KHRONOS_validation"); }

    VkApplicationInfo applicationInfo{};
    applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    applicationInfo.pApplicationName = "mcvr-layer3a-smoke";
    applicationInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    applicationInfo.pEngineName = "mcvr-layer3a";
    applicationInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    const uint32_t targetApi = VK_API_VERSION_1_3;
    applicationInfo.apiVersion = (loaderVersion >= targetApi) ? targetApi : loaderVersion;
    evidence.addStr("instance_api_version", std::to_string(VK_VERSION_MAJOR(applicationInfo.apiVersion)) + "." +
                                                std::to_string(VK_VERSION_MINOR(applicationInfo.apiVersion)) + "." +
                                                std::to_string(VK_VERSION_PATCH(applicationInfo.apiVersion)));

    VkDebugUtilsMessengerCreateInfoEXT messengerInfo{};
    messengerInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    messengerInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    messengerInfo.pfnUserCallback = validationCallback;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &applicationInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();
    createInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
    createInfo.ppEnabledLayerNames = layers.data();
    if (debugUtils) { createInfo.pNext = &messengerInfo; }

    requireVk(vkCreateInstance(&createInfo, nullptr, &resources.instance), "instance_create", "vkCreateInstance");

    if (requireValidation) {
        auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(resources.instance, "vkCreateDebugUtilsMessengerEXT"));
        if (createMessenger == nullptr) {
            failWith("validation_error_channel_missing", "vkCreateDebugUtilsMessengerEXT is null");
        }
        requireVk(createMessenger(resources.instance, &messengerInfo, nullptr, &resources.messenger),
                  "validation_messenger_create", "vkCreateDebugUtilsMessengerEXT");

        uint32_t layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> layersFound(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, layersFound.data());
        for (const auto &layer : layersFound) {
            if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                evidence.addStr("validation_layer_spec_version", std::to_string(VK_API_VERSION_MAJOR(layer.specVersion)) +
                                                                     "." +
                                                                     std::to_string(VK_API_VERSION_MINOR(layer.specVersion)) +
                                                                     "." +
                                                                     std::to_string(VK_API_VERSION_PATCH(layer.specVersion)));
            }
        }
        evidence.addBool("validation_enabled", true);
    } else {
        evidence.addBool("validation_enabled", false);
    }
    return resources;
}

void destroyInstance(InstanceResources &resources) {
    if (resources.messenger != VK_NULL_HANDLE && resources.instance != VK_NULL_HANDLE) {
        auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(resources.instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyMessenger != nullptr) { destroyMessenger(resources.instance, resources.messenger, nullptr); }
        resources.messenger = VK_NULL_HANDLE;
    }
    if (resources.instance != VK_NULL_HANDLE) {
        vkDestroyInstance(resources.instance, nullptr);
        resources.instance = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// Device enumeration
// ---------------------------------------------------------------------------

struct DeviceInfo {
    VkPhysicalDevice handle = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    CapabilityReport capabilities;
    bool admitted = false;
};

std::string deviceKind(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_OTHER: return "other";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated-gpu";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete-gpu";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual-gpu";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
    default: return "unknown";
    }
}

std::string driverVersionString(uint32_t version) {
    return std::to_string(VK_VERSION_MAJOR(version)) + "." + std::to_string(VK_VERSION_MINOR(version)) + "." +
           std::to_string(VK_VERSION_PATCH(version));
}

std::vector<DeviceInfo> enumerateDevices(VkInstance instance, Admission admission,
                                         const CapabilityMask &requiredMask,
                                         const std::string &expectedDeviceName) {
    uint32_t deviceCount = 0;
    if (vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr) != VK_SUCCESS || deviceCount == 0) {
        failWith("device_enumerate", "no Vulkan physical devices visible");
    }
    std::vector<VkPhysicalDevice> handles(deviceCount);
    requireVk(vkEnumeratePhysicalDevices(instance, &deviceCount, handles.data()), "device_enumerate",
              "vkEnumeratePhysicalDevices");

    std::vector<DeviceInfo> devices;
    devices.reserve(handles.size());
    for (VkPhysicalDevice handle : handles) {
        DeviceInfo info;
        info.handle = handle;
        vkGetPhysicalDeviceProperties(handle, &info.properties);
        info.capabilities = mcvr::device_policy::evaluateCapabilities(handle);
        info.admitted = info.capabilities.suitable(requiredMask) &&
                        mcvr::device_policy::isDeviceAdmitted(info.properties, admission);
        if (info.admitted && !expectedDeviceName.empty()) {
            info.admitted = std::string(info.properties.deviceName).find(expectedDeviceName) != std::string::npos;
        }
        devices.push_back(info);
    }
    return devices;
}

std::string devicesJson(const std::vector<DeviceInfo> &devices) {
    std::ostringstream out;
    out << "[";
    bool first = true;
    for (const auto &device : devices) {
        if (!first) { out << ","; }
        first = false;
        out << "{"
            << "\"name\":" << jstr(device.properties.deviceName) << ","
            << "\"type\":" << jstr(deviceKind(device.properties.deviceType)) << ","
            << "\"vendor_id\":" << ju32(device.properties.vendorID) << ","
            << "\"device_id\":" << ju32(device.properties.deviceID) << ","
            << "\"driver_version\":" << jstr(driverVersionString(device.properties.driverVersion)) << ","
            << "\"api_version\":" << jstr(driverVersionString(device.properties.apiVersion)) << ","
            << "\"capabilities\":{"
            << "\"swapchain\":" << jbool(device.capabilities.swapchain) << ","
            << "\"ray_tracing_pipeline\":" << jbool(device.capabilities.rayTracingPipeline) << ","
            << "\"buffer_device_address\":" << jbool(device.capabilities.bufferDeviceAddress) << ","
            << "\"synchronization2\":" << jbool(device.capabilities.synchronization2) << "},"
            << "\"admitted\":" << jbool(device.admitted) << "}";
    }
    out << "]";
    return out.str();
}

struct QueueFamilyIndices {
    int graphics = -1;
    int compute = -1;
    int transfer = -1;
    int present = -1;

    bool complete(bool withSurface) const {
        return graphics >= 0 && (!withSurface || present >= 0);
    }
};

QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface, bool withSurface) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

    QueueFamilyIndices indices;
    for (uint32_t i = 0; i < count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
            indices.graphics = i;
            if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0) { indices.compute = i; }
            if ((families[i].queueFlags & VK_QUEUE_TRANSFER_BIT) != 0) { indices.transfer = i; }
        }
        if (withSurface && surface != VK_NULL_HANDLE) {
            VkBool32 presentSupport = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);
            if (presentSupport == VK_TRUE && indices.present < 0) { indices.present = static_cast<int>(i); }
        }
    }
    return indices;
}

// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------

struct Options {
    std::string mode;
    bool requireValidation = false;
    bool allowCpu = false;
    std::string expectedDeviceName;
    std::string simulateMissing;
};

int runPreflight(const Options &options) {
    Evidence evidence;
    evidence.mode = "preflight";
    evidence.addStr("admission", mcvr::device_policy::admissionName(options.allowCpu
                                                                       ? Admission::TestAllowCpuDevice
                                                                       : Admission::ProductionDiscreteOnly));
    InstanceResources resources = createInstance(options.requireValidation, false, evidence);
    evidence.terminalPhase = "instance";

    const Admission admission =
        options.allowCpu ? Admission::TestAllowCpuDevice : Admission::ProductionDiscreteOnly;
    std::vector<DeviceInfo> devices =
        enumerateDevices(resources.instance, admission, CapabilityMask{}, options.expectedDeviceName);

    if (!options.simulateMissing.empty()) {
        // Capability-fixture path: mark the named capability unavailable on every
        // device and require the selector to fail closed instead of admitting it.
        for (auto &device : devices) {
            if (options.simulateMissing == "swapchain") { device.capabilities.swapchain = false; }
            if (options.simulateMissing == "raytracing") { device.capabilities.rayTracingPipeline = false; }
            if (options.simulateMissing == "buffer-device-address") { device.capabilities.bufferDeviceAddress = false; }
            if (options.simulateMissing == "synchronization2") { device.capabilities.synchronization2 = false; }
            device.admitted = device.capabilities.suitable() &&
                              mcvr::device_policy::isDeviceAdmitted(device.properties, admission);
        }
    }

    evidence.add("devices", devicesJson(devices));
    evidence.addStr("simulated_missing_capability",
                    options.simulateMissing.empty() ? "none" : options.simulateMissing);
    evidence.addU32("device_count", static_cast<uint32_t>(devices.size()));

    bool anyAdmitted = false;
    for (const auto &device : devices) { anyAdmitted = anyAdmitted || device.admitted; }

    if (!anyAdmitted) {
        evidence.ok = false;
        evidence.terminalPhase = "no_admitted_device";
        evidence.addStr("error", "no visible device satisfies the admitted capability contract");
        emit(evidence);
        destroyInstance(resources);
        return 1;
    }

    evidence.ok = true;
    evidence.terminalPhase = "preflight_complete";
    emit(evidence);
    destroyInstance(resources);
    return 0;
}

int runProdReject(const Options &options) {
    Evidence evidence;
    evidence.mode = "prod-reject";
    evidence.addStr("admission", mcvr::device_policy::admissionName(Admission::ProductionDiscreteOnly));
    // The production-policy target always evaluates the shipping policy. A CPU
    // admission request is recorded and refused rather than silently honored.
    evidence.addBool("cpu_admission_request_refused", options.allowCpu);

    InstanceResources resources = createInstance(false, false, evidence);
    std::vector<DeviceInfo> devices =
        enumerateDevices(resources.instance, Admission::ProductionDiscreteOnly, CapabilityMask{}, "");
    evidence.add("devices", devicesJson(devices));
    evidence.terminalPhase = "device_enumerate";

    bool anyAdmitted = false;
    for (const auto &device : devices) { anyAdmitted = anyAdmitted || device.admitted; }

    if (anyAdmitted) {
        evidence.ok = false;
        evidence.terminalPhase = "production_policy_admitted_device";
        emit(evidence);
        destroyInstance(resources);
        return 4;
    }

    evidence.ok = true;
    evidence.terminalPhase = "production_policy_rejected_all_visible_devices";
    emit(evidence);
    destroyInstance(resources);
    return 3;
}

int runCanary(const Options &options) {
    Evidence evidence;
    evidence.mode = "canary";
    if (!options.requireValidation) { failWith("canary_requires_validation", "--canary requires --require-validation"); }
    InstanceResources resources = createInstance(true, false, evidence);
    evidence.terminalPhase = "instance";

    const Admission admission =
        options.allowCpu ? Admission::TestAllowCpuDevice : Admission::ProductionDiscreteOnly;
    std::vector<DeviceInfo> devices =
        enumerateDevices(resources.instance, admission, CapabilityMask{}, options.expectedDeviceName);
    VkPhysicalDevice selected = VK_NULL_HANDLE;
    for (const auto &device : devices) {
        if (device.admitted) {
            selected = device.handle;
            evidence.addStr("device_name", device.properties.deviceName);
            break;
        }
    }
    if (selected == VK_NULL_HANDLE) { failWith("no_admitted_device", "canary found no admitted device"); }

    QueueFamilyIndices indices = findQueueFamilies(selected, VK_NULL_HANDLE, false);
    if (indices.graphics < 0) { failWith("queue_family", "no graphics queue family"); }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = static_cast<uint32_t>(indices.graphics);
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;

    VkDevice device = VK_NULL_HANDLE;
    requireVk(vkCreateDevice(selected, &deviceInfo, nullptr, &device), "device_create", "vkCreateDevice");
    evidence.terminalPhase = "device";

    // Deterministic invalid call: VkBufferCreateInfo::usage must be non-zero.
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = 1;
    bufferInfo.usage = 0;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkResult canaryResult = vkCreateBuffer(device, &bufferInfo, nullptr, &buffer);
    evidence.addStr("canary_call_result", vkResultName(canaryResult));
    if (buffer != VK_NULL_HANDLE) { vkDestroyBuffer(device, buffer, nullptr); }
    vkDestroyDevice(device, nullptr);

    evidence.addU32("validation_errors", gValidation != nullptr ? gValidation->errors : 0);
    if (gValidation != nullptr && !gValidation->firstError.empty()) {
        evidence.addStr("first_validation_error", gValidation->firstError);
    }
    const bool detected = gValidation != nullptr && gValidation->errors > 0;
    evidence.ok = detected;
    evidence.terminalPhase = detected ? "canary_detected_validation_error" : "canary_missed_validation_error";
    emit(evidence);
    destroyInstance(resources);
    return detected ? 0 : 1;
}

int runSmoke(const Options &options) {
    Evidence evidence;
    evidence.mode = "smoke";
    evidence.addStr("admission", mcvr::device_policy::admissionName(options.allowCpu
                                                                       ? Admission::TestAllowCpuDevice
                                                                       : Admission::ProductionDiscreteOnly));

#if GLFW_VERSION_MAJOR > 3 || (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 4)
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
#endif
    if (glfwInit() != GLFW_TRUE) { failWith("glfw_init", "glfwInit failed (is the X server reachable?)"); }
    evidence.addStr("glfw_version", glfwGetVersionString());
#if GLFW_VERSION_MAJOR > 3 || (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 4)
    const int platform = glfwGetPlatform();
    evidence.addStr("glfw_platform", platform == GLFW_PLATFORM_X11 ? "x11" : std::to_string(platform));
    if (platform != GLFW_PLATFORM_X11) {
        glfwTerminate();
        failWith("glfw_platform", "GLFW did not select the X11 platform");
    }
#else
    evidence.addStr("glfw_platform", "x11 (GLFW < 3.4: X11 is the only Linux platform)");
#endif
    const char *display = std::getenv("DISPLAY");
    const char *xauthority = std::getenv("XAUTHORITY");
    evidence.addStr("display", display != nullptr ? display : "");
    evidence.addStr("xauthority", xauthority != nullptr ? xauthority : "");
    if (display == nullptr || *display == '\0') {
        glfwTerminate();
        failWith("display_missing", "DISPLAY is not set");
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    GLFWwindow *window = glfwCreateWindow(static_cast<int>(kWindowWidth), static_cast<int>(kWindowHeight),
                                          "mcvr-layer3a", nullptr, nullptr);
    if (window == nullptr) {
        glfwTerminate();
        failWith("glfw_window", "glfwCreateWindow failed");
    }
    evidence.terminalPhase = "glfw_window";

    InstanceResources resources = createInstance(options.requireValidation, true, evidence);
    evidence.terminalPhase = "instance";

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    requireVk(glfwCreateWindowSurface(resources.instance, window, nullptr, &surface), "surface_create",
              "glfwCreateWindowSurface");
    evidence.terminalPhase = "surface";

    const Admission admission =
        options.allowCpu ? Admission::TestAllowCpuDevice : Admission::ProductionDiscreteOnly;
    std::vector<DeviceInfo> devices =
        enumerateDevices(resources.instance, admission, CapabilityMask{}, options.expectedDeviceName);
    evidence.add("devices", devicesJson(devices));

    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    for (const auto &device : devices) {
        if (device.admitted) {
            physicalDevice = device.handle;
            properties = device.properties;
            break;
        }
    }
    if (physicalDevice == VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(resources.instance, surface, nullptr);
        destroyInstance(resources);
        glfwDestroyWindow(window);
        glfwTerminate();
        failWith("no_admitted_device", "no device satisfies the admitted policy and surface path");
    }

    evidence.addStr("device_name", properties.deviceName);
    evidence.addStr("device_type", deviceKind(properties.deviceType));
    evidence.addU32("device_vendor_id", properties.vendorID);
    evidence.addU32("device_id", properties.deviceID);
    evidence.addStr("device_driver_version", driverVersionString(properties.driverVersion));
    evidence.addStr("device_api_version", driverVersionString(properties.apiVersion));

    QueueFamilyIndices indices = findQueueFamilies(physicalDevice, surface, true);
    if (indices.graphics < 0 || indices.present < 0) {
        vkDestroySurfaceKHR(resources.instance, surface, nullptr);
        destroyInstance(resources);
        glfwDestroyWindow(window);
        glfwTerminate();
        failWith("queue_family", "no graphics+present queue family for the smoke surface");
    }
    evidence.addU32("graphics_queue_family", static_cast<uint32_t>(indices.graphics));
    evidence.addU32("present_queue_family", static_cast<uint32_t>(indices.present));
    evidence.addBool("compute_queue_supported", indices.compute >= 0);
    evidence.addBool("transfer_queue_supported", indices.transfer >= 0);
    evidence.terminalPhase = "queue_family";

    VkSurfaceCapabilitiesKHR capabilities{};
    requireVk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &capabilities),
              "surface_capabilities", "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr);
    if (formatCount == 0) {
        vkDestroySurfaceKHR(resources.instance, surface, nullptr);
        destroyInstance(resources);
        glfwDestroyWindow(window);
        glfwTerminate();
        failWith("surface_formats", "surface reports no formats");
    }
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    requireVk(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, formats.data()),
              "surface_formats", "vkGetPhysicalDeviceSurfaceFormatsKHR");

    VkSurfaceFormatKHR chosenFormat = formats[0];
    for (const auto &format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosenFormat = format;
            break;
        }
    }

    uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount, nullptr);
    if (presentModeCount == 0) {
        vkDestroySurfaceKHR(resources.instance, surface, nullptr);
        destroyInstance(resources);
        glfwDestroyWindow(window);
        glfwTerminate();
        failWith("present_modes", "surface reports no present modes");
    }
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    requireVk(vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount,
                                                        presentModes.data()),
              "present_modes", "vkGetPhysicalDeviceSurfacePresentModesKHR");

    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX || extent.height == UINT32_MAX) {
        extent.width = std::clamp(kWindowWidth, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height = std::clamp(kWindowHeight, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    }

    evidence.addStr("swapchain_format", std::to_string(chosenFormat.format));
    evidence.addStr("swapchain_color_space", std::to_string(chosenFormat.colorSpace));
    evidence.addStr("swapchain_present_mode", "fifo");
    evidence.addStr("swapchain_extent", std::to_string(extent.width) + "x" + std::to_string(extent.height));
    evidence.addU32("swapchain_min_image_count", capabilities.minImageCount);
    evidence.terminalPhase = "surface_capabilities";

    const float priority = 1.0f;
    std::vector<uint32_t> uniqueFamilies{static_cast<uint32_t>(indices.graphics)};
    if (indices.present != indices.graphics) { uniqueFamilies.push_back(static_cast<uint32_t>(indices.present)); }
    std::vector<VkDeviceQueueCreateInfo> queueInfos(uniqueFamilies.size());
    for (size_t i = 0; i < uniqueFamilies.size(); ++i) {
        queueInfos[i].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfos[i].queueFamilyIndex = uniqueFamilies[i];
        queueInfos[i].queueCount = 1;
        queueInfos[i].pQueuePriorities = &priority;
    }

    const char *deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceVulkan12Features enabled12{};
    enabled12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    enabled12.bufferDeviceAddress = VK_TRUE;
    VkPhysicalDeviceVulkan13Features enabled13{};
    enabled13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    enabled13.synchronization2 = VK_TRUE;
    enabled13.pNext = &enabled12;

    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = &enabled13;
    deviceInfo.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
    deviceInfo.pQueueCreateInfos = queueInfos.data();
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = deviceExtensions;

    VkDevice device = VK_NULL_HANDLE;
    VkResult createDeviceResult = vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device);
    if (createDeviceResult != VK_SUCCESS) {
        vkDestroySurfaceKHR(resources.instance, surface, nullptr);
        destroyInstance(resources);
        glfwDestroyWindow(window);
        glfwTerminate();
        failWith("device_create", "vkCreateDevice returned " + vkResultName(createDeviceResult));
    }
    evidence.terminalPhase = "device";

    uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
        imageCount = capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR swapchainInfo{};
    swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchainInfo.surface = surface;
    swapchainInfo.minImageCount = imageCount;
    swapchainInfo.imageFormat = chosenFormat.format;
    swapchainInfo.imageColorSpace = chosenFormat.colorSpace;
    swapchainInfo.imageExtent = extent;
    swapchainInfo.imageArrayLayers = 1;
    swapchainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (indices.present != indices.graphics) {
        swapchainInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        swapchainInfo.queueFamilyIndexCount = static_cast<uint32_t>(uniqueFamilies.size());
        swapchainInfo.pQueueFamilyIndices = uniqueFamilies.data();
    } else {
        swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    swapchainInfo.preTransform = capabilities.currentTransform;
    swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if ((capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) == 0) {
        swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    }
    swapchainInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchainInfo.clipped = VK_TRUE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkResult createSwapchainResult = vkCreateSwapchainKHR(device, &swapchainInfo, nullptr, &swapchain);
    if (createSwapchainResult != VK_SUCCESS) {
        vkDestroyDevice(device, nullptr);
        vkDestroySurfaceKHR(resources.instance, surface, nullptr);
        destroyInstance(resources);
        glfwDestroyWindow(window);
        glfwTerminate();
        failWith("swapchain_create", "vkCreateSwapchainKHR returned " + vkResultName(createSwapchainResult));
    }

    uint32_t swapchainImageCount = 0;
    requireVk(vkGetSwapchainImagesKHR(device, swapchain, &swapchainImageCount, nullptr), "swapchain_images",
              "vkGetSwapchainImagesKHR");
    std::vector<VkImage> images(swapchainImageCount);
    requireVk(vkGetSwapchainImagesKHR(device, swapchain, &swapchainImageCount, images.data()), "swapchain_images",
              "vkGetSwapchainImagesKHR");
    evidence.addU32("swapchain_image_count", swapchainImageCount);
    evidence.terminalPhase = "swapchain";

    vkDestroySwapchainKHR(device, swapchain, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroySurfaceKHR(resources.instance, surface, nullptr);
    destroyInstance(resources);
    glfwDestroyWindow(window);
    glfwTerminate();
    evidence.terminalPhase = "destroy";

    const uint32_t validationErrors = gValidation != nullptr ? gValidation->errors : 0;
    evidence.addU32("validation_errors", validationErrors);
    if (validationErrors > 0) {
        evidence.ok = false;
        evidence.terminalPhase = "validation_errors";
        if (gValidation != nullptr) { evidence.addStr("first_validation_error", gValidation->firstError); }
        emit(evidence);
        return 1;
    }

    evidence.ok = true;
    emit(evidence);
    return 0;
}

Options parseOptions(int argc, char **argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--preflight" || argument == "--smoke" || argument == "--canary" ||
            argument == "--prod-reject") {
            options.mode = argument.substr(2);
        } else if (argument == "--require-validation") {
            options.requireValidation = true;
        } else if (argument == "--allow-cpu") {
            options.allowCpu = true;
        } else if (argument == "--emit") {
            if (i + 1 >= argc) { failWith("arguments", "--emit requires a path"); }
            gEmitPath = argv[++i];
        } else if (argument == "--expect-device-name") {
            if (i + 1 >= argc) { failWith("arguments", "--expect-device-name requires a value"); }
            options.expectedDeviceName = argv[++i];
        } else if (argument == "--simulate-missing") {
            if (i + 1 >= argc) { failWith("arguments", "--simulate-missing requires a value"); }
            options.simulateMissing = argv[++i];
        } else {
            failWith("arguments", "unknown argument: " + argument);
        }
    }
    if (options.mode.empty()) { failWith("arguments", "one of --preflight/--smoke/--canary/--prod-reject is required"); }
    return options;
}

} // namespace

int main(int argc, char **argv) {
    ValidationState validation;
    gValidation = &validation;

    Options options = parseOptions(argc, argv);

    if (options.allowCpu) {
#ifndef MCVR_LAYER3A_TEST_CPU_ADMISSION
        failWith("cpu_admission_not_compiled_in",
                 "this binary was built without MCVR_LAYER3A_TEST_CPU_ADMISSION; CPU admission is test-only");
#endif
    }
    validation.required = options.requireValidation;

    if (options.mode == "preflight") { return runPreflight(options); }
    if (options.mode == "prod-reject") { return runProdReject(options); }
    if (options.mode == "canary") { return runCanary(options); }
    return runSmoke(options);
}
