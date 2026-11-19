#include "core/vulkan/physical_device.hpp"

#include "core/vulkan/device_policy.hpp"
#include "core/vulkan/instance.hpp"
#include "core/vulkan/window.hpp"

#include <iostream>
#include <vector>

std::ostream &physicalDeviceCout() {
    return std::cout << "[PhysicalDevice] ";
}

std::ostream &physicalDeviceCerr() {
    return std::cerr << "[PhysicalDevice] ";
}

void vk::PhysicalDevice::findPhysicalDevice() {
    uint32_t deviceCount = 0;
    if (vkEnumeratePhysicalDevices(instance_->vkInstance(), &deviceCount, nullptr) != VK_SUCCESS || deviceCount == 0) {
        physicalDeviceCerr() << "failed to get number of physical devices" << std::endl;
        exit(1);
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    if (vkEnumeratePhysicalDevices(instance_->vkInstance(), &deviceCount, devices.data()) != VK_SUCCESS) {
        physicalDeviceCerr() << "failed to retrieve physical devices" << std::endl;
        exit(1);
    }

    // find the first supported physical device
    for (const auto &device : devices) {
        if (mcvr::device_policy::isDeviceSuitable(device)) {
            VkPhysicalDeviceProperties properties;
            vkGetPhysicalDeviceProperties(device, &properties);

            if (!mcvr::device_policy::isDeviceAdmitted(properties, mcvr::device_policy::Admission::ProductionDiscreteOnly)) {
                continue;
            }

            physicalDevice_ = device;
#ifdef DEBUG
            physicalDeviceCout() << "found suitable physical device" << std::endl;
#endif

            // output device info
            uint32_t supportedVersion[] = {VK_VERSION_MAJOR(properties.apiVersion),
                                           VK_VERSION_MINOR(properties.apiVersion),
                                           VK_VERSION_PATCH(properties.apiVersion)};

#ifdef DEBUG
            physicalDeviceCout() << "selected device name: " << properties.deviceName << std::endl;
            physicalDeviceCout() << "supports Vulkan version: " << supportedVersion[0] << "." << supportedVersion[1]
                                 << "." << supportedVersion[2] << std::endl;
#endif

            return;
        }
    }

    // if no supported physical device is found
    physicalDeviceCerr() << "No suitable physical device found!" << std::endl;
    exit(EXIT_FAILURE);
}

vk::PhysicalDevice::PhysicalDevice(std::shared_ptr<Instance> instance, std::shared_ptr<Window> window)
    : instance_(instance), window_(window) {
    findPhysicalDevice();
    findQueueFamilies();

    VkPhysicalDeviceAccelerationStructurePropertiesKHR accelStructProperties{};
    accelStructProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;

    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rayTracingProperties{};
    rayTracingProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
    rayTracingProperties.pNext = &accelStructProperties;

    VkPhysicalDeviceProperties2 deviceProps2{};
    deviceProps2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    deviceProps2.pNext = &rayTracingProperties;
    vkGetPhysicalDeviceProperties2(physicalDevice_, &deviceProps2);

    properties_ = deviceProps2.properties;
    rayTracingProperties_ = rayTracingProperties;
    accelerationStructProperties_ = accelStructProperties;
}

vk::PhysicalDevice::~PhysicalDevice() {
#ifdef DEBUG
    physicalDeviceCout() << "physical device deconstructed" << std::endl;
#endif
}

VkPhysicalDevice &vk::PhysicalDevice::vkPhysicalDevice() {
    return physicalDevice_;
}

uint32_t vk::PhysicalDevice::mainQueueIndex() {
    return mainQueueIndex_;
}

uint32_t vk::PhysicalDevice::secondaryQueueIndex() {
    return secondaryQueueIndex_;
}

void vk::PhysicalDevice::findQueueFamilies() {
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &queueFamilyCount, nullptr);

    if (queueFamilyCount == 0) {
        physicalDeviceCerr() << "Physical device has no queue families!" << std::endl;
        exit(EXIT_FAILURE);
    }

    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &queueFamilyCount, queueFamilies.data());

#ifdef DEBUG
    physicalDeviceCout() << "physical device has " << queueFamilyCount << " queue families" << std::endl;
#endif

    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        VkBool32 presentSupport = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice_, i, window_->vkSurface(), &presentSupport);

        VkBool32 graphicsSupport = false;
        if (queueFamilies[i].queueCount > 0 && (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            graphicsSupport = true;
        }

        VkBool32 computeSupport = false;
        if (queueFamilies[i].queueCount > 0 && (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            computeSupport = true;
        }

        VkBool32 transferSupport = false;
        if (queueFamilies[i].queueCount > 0 && (queueFamilies[i].queueFlags & VK_QUEUE_TRANSFER_BIT)) {
            transferSupport = true;
        }

        // Early exit if all needed queue families are found
        if (presentSupport && graphicsSupport && computeSupport && transferSupport) {
            mainQueueIndex_ = i;
            // TODO: add more condition
            secondaryQueueIndex_ = i;
            break;
        }
    }

    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        VkBool32 presentSupport = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice_, i, window_->vkSurface(), &presentSupport);

        VkBool32 graphicsSupport = false;
        if (queueFamilies[i].queueCount > 0 && (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            graphicsSupport = true;
        }

        VkBool32 computeSupport = false;
        if (queueFamilies[i].queueCount > 0 && (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            computeSupport = true;
        }

        VkBool32 transferSupport = false;
        if (queueFamilies[i].queueCount > 0 && (queueFamilies[i].queueFlags & VK_QUEUE_TRANSFER_BIT)) {
            transferSupport = true;
        }

        // Early exit if all needed queue families are found
        if (computeSupport && transferSupport && i != mainQueueIndex_) {
            secondaryQueueIndex_ = i;
            break;
        }
    }

    if (mainQueueIndex_ == -1) {
        physicalDeviceCerr() << "No queue family that supports graphics, compute and transfer found." << std::endl;
        exit(EXIT_FAILURE);
    }

    if (secondaryQueueIndex_ == -1) {
        physicalDeviceCerr() << "No queue family that supports graphics, compute and transfer found." << std::endl;
        exit(EXIT_FAILURE);
    }
}

VkPhysicalDeviceProperties vk::PhysicalDevice::properties() {
    return properties_;
}

VkPhysicalDeviceRayTracingPipelinePropertiesKHR vk::PhysicalDevice::rayTracingProperties() {
    return rayTracingProperties_;
}

VkPhysicalDeviceAccelerationStructurePropertiesKHR vk::PhysicalDevice::accelerationStructProperties() {
    return accelerationStructProperties_;
}
