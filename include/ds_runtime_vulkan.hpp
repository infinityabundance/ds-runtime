// SPDX-License-Identifier: Apache-2.0
// Vulkan backend interface for ds-runtime.

#pragma once

#include "ds_runtime.hpp"

#ifdef DS_RUNTIME_HAS_VULKAN
#include <vulkan/vulkan.h>
#else
struct VkInstance_T;
struct VkPhysicalDevice_T;
struct VkDevice_T;
struct VkQueue_T;
struct VkCommandPool_T;
using VkInstance = VkInstance_T*;
using VkPhysicalDevice = VkPhysicalDevice_T*;
using VkDevice = VkDevice_T*;
using VkQueue = VkQueue_T*;
using VkCommandPool = VkCommandPool_T*;
#endif

namespace ds {

/// Configuration for the Vulkan backend.
///
/// If device/queue/command_pool are provided, the backend will use them
/// without taking ownership. Otherwise it will create its own Vulkan context.
struct VulkanBackendConfig {
    VkInstance       instance          = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device   = VK_NULL_HANDLE;
    VkDevice         device            = VK_NULL_HANDLE;
    VkQueue          queue             = VK_NULL_HANDLE;
    std::uint32_t    queue_family_index = 0;
    VkCommandPool    command_pool      = VK_NULL_HANDLE;
    std::size_t      worker_count      = 1;
};

/// Create a Vulkan-backed implementation.
std::shared_ptr<Backend> make_vulkan_backend(const VulkanBackendConfig& config);

} // namespace ds
