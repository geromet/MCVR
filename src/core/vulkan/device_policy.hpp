#pragma once

// Single source of truth for MCVR's physical-device admission policy.
//
// The production renderer may only select a discrete GPU. The Layer-3A
// software-Vulkan verification target needs the exact same capability contract
// but admits a CPU (LavaPipe) device. Keeping both decisions in this header means
// the verification target exercises the production policy code instead of a copy
// of it, and that CPU admission is a separate, explicitly named policy value.

#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace mcvr::device_policy {

enum class Admission {
    // Shipping renderer policy: discrete GPU only.
    ProductionDiscreteOnly,
    // Verification-only policy: the exact CPU/LavaPipe device. It never admits
    // an integrated or discrete hardware GPU, so a host GPU cannot manufacture a
    // software-Vulkan PASS.
    TestAllowCpuDevice,
};

inline const char *admissionName(Admission admission) {
    switch (admission) {
    case Admission::ProductionDiscreteOnly: return "production-discrete-only";
    case Admission::TestAllowCpuDevice: return "test-only-cpu-admission";
    }
    return "unknown";
}

// Capabilities currently consulted by the renderer's device selection.
struct CapabilityMask {
    bool requireSwapchain = true;
    bool requireRayTracingPipeline = true;
    bool requireBufferDeviceAddress = true;
    bool requireSynchronization2 = true;
};

struct CapabilityReport {
    bool swapchain = false;
    bool rayTracingPipeline = false;
    bool bufferDeviceAddress = false;
    bool synchronization2 = false;

    bool suitable(const CapabilityMask &required = {}) const {
        return (!required.requireSwapchain || swapchain) &&
               (!required.requireRayTracingPipeline || rayTracingPipeline) &&
               (!required.requireBufferDeviceAddress || bufferDeviceAddress) &&
               (!required.requireSynchronization2 || synchronization2);
    }
};

// Enumerates the device extensions/features that MCVR's isDeviceSuitable()
// contract depends on. Must be called after the instance is created.
inline CapabilityReport evaluateCapabilities(VkPhysicalDevice device) {
    CapabilityReport report;

    uint32_t extensionCount = 0;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr) != VK_SUCCESS) {
        return report;
    }
    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data()) !=
        VK_SUCCESS) {
        return report;
    }

    for (const auto &ext : availableExtensions) {
        const std::string name(ext.extensionName);
        if (name == VK_KHR_SWAPCHAIN_EXTENSION_NAME) { report.swapchain = true; }
        if (name == VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) { report.rayTracingPipeline = true; }
    }

    VkPhysicalDeviceVulkan12Features vulkan12Features{};
    vulkan12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;

    VkPhysicalDeviceVulkan13Features vulkan13Features{};
    vulkan13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    vulkan13Features.pNext = &vulkan12Features;

    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructureFeatures{};
    accelerationStructureFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    accelerationStructureFeatures.pNext = &vulkan13Features;

    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracingFeatures{};
    rayTracingFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
    rayTracingFeatures.pNext = &accelerationStructureFeatures;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &rayTracingFeatures;

    vkGetPhysicalDeviceFeatures2(device, &features2);

    report.rayTracingPipeline = report.rayTracingPipeline && rayTracingFeatures.rayTracingPipeline &&
                                accelerationStructureFeatures.accelerationStructure;
    report.bufferDeviceAddress = vulkan12Features.bufferDeviceAddress == VK_TRUE;
    report.synchronization2 = vulkan13Features.synchronization2 == VK_TRUE;
    return report;
}

inline bool isDeviceSuitable(VkPhysicalDevice device, const CapabilityMask &required = {}) {
    return evaluateCapabilities(device).suitable(required);
}

inline bool isDeviceAdmitted(const VkPhysicalDeviceProperties &properties, Admission admission) {
    switch (admission) {
    case Admission::ProductionDiscreteOnly:
        return properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    case Admission::TestAllowCpuDevice:
        return properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    }
    return false;
}

} // namespace mcvr::device_policy
