#include <vulkan/vulkan.h>
#include <iostream>
#include <vector>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <cstdlib>

#define VK_CHECK(x)                                                     \
    do {                                                                \
        VkResult err__ = (x);                                           \
        if (err__ != VK_SUCCESS) {                                      \
            std::cerr << "Vulkan error " << err__                       \
                      << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
            std::exit(1);                                               \
        }                                                               \
    } while (0)

static std::vector<char> readFile(const char* path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file) throw std::runtime_error(std::string("Failed to open file: ") + path);
    size_t size = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(size);
    file.seekg(0);
    file.read(buffer.data(), size);
    return buffer;
}

uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties, VkPhysicalDevice physDev) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physDev, &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    throw std::runtime_error("Failed to find suitable memory type");
}

int main() {
    // 1. Create Vulkan instance
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "VK Copy Test";
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &appInfo;

    VkInstance instance;
    VK_CHECK(vkCreateInstance(&ici, nullptr, &instance));

    // 2. Pick a physical device
    uint32_t gpuCount = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &gpuCount, nullptr));
    if (gpuCount == 0) {
        std::cerr << "No Vulkan-capable GPU found.\n";
        return 1;
    }
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &gpuCount, gpus.data()));
    VkPhysicalDevice physDev = gpus[0]; // just pick the first one

    // 3. Find a compute-capable queue family
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physDev, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> qprops(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physDev, &queueFamilyCount, qprops.data());

    uint32_t computeQueueFamily = UINT32_MAX;
    for (uint32_t i = 0; i < queueFamilyCount; ++i) {
        if (qprops[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            computeQueueFamily = i;
            break;
        }
    }
    if (computeQueueFamily == UINT32_MAX) {
        std::cerr << "No compute-capable queue family found.\n";
        return 1;
    }

    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo dqci{};
    dqci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    dqci.queueFamilyIndex = computeQueueFamily;
    dqci.queueCount = 1;
    dqci.pQueuePriorities = &queuePriority;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &dqci;

    VkDevice device;
    VK_CHECK(vkCreateDevice(physDev, &dci, nullptr, &device));

    VkQueue queue;
    vkGetDeviceQueue(device, computeQueueFamily, 0, &queue);

    // 4. Create a command pool
    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = computeQueueFamily;

    VkCommandPool cmdPool;
    VK_CHECK(vkCreateCommandPool(device, &cpci, nullptr, &cmdPool));

    // 5. Create two small buffers (64 bytes each)
    const VkDeviceSize bufferSize = 64; // bytes

    auto createBuffer = [&](VkBuffer& buf, VkDeviceMemory& mem) {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = bufferSize;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VK_CHECK(vkCreateBuffer(device, &bci, nullptr, &buf));

        VkMemoryRequirements memReq{};
        vkGetBufferMemoryRequirements(device, buf, &memReq);

        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = memReq.size;
        mai.memoryTypeIndex = findMemoryType(
            memReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            physDev
        );

        VK_CHECK(vkAllocateMemory(device, &mai, nullptr, &mem));
        VK_CHECK(vkBindBufferMemory(device, buf, mem, 0));
    };

    VkBuffer srcBuf, dstBuf;
    VkDeviceMemory srcMem, dstMem;
    createBuffer(srcBuf, srcMem);
    createBuffer(dstBuf, dstMem);

    // 6. Fill src buffer with a string (and zero the rest)
    {
        void* p = nullptr;
        VK_CHECK(vkMapMemory(device, srcMem, 0, bufferSize, 0, &p));
        char* c = static_cast<char*>(p);
        const char* msg = "Hello from Vulkan compute!";
        size_t len = std::strlen(msg);
        if (len >= bufferSize) len = bufferSize - 1;
        std::memset(c, 0, bufferSize);
        std::memcpy(c, msg, len);
        vkUnmapMemory(device, srcMem);
    }

    // Also clear dst buffer
    {
        void* p = nullptr;
        VK_CHECK(vkMapMemory(device, dstMem, 0, bufferSize, 0, &p));
        std::memset(p, 0, bufferSize);
        vkUnmapMemory(device, dstMem);
    }

    // 7. Load compute shader module
    std::vector<char> shaderCode = readFile("copy.comp.spv");

    VkShaderModuleCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = shaderCode.size();
    smci.pCode = reinterpret_cast<const uint32_t*>(shaderCode.data());

    VkShaderModule shaderModule;
    VK_CHECK(vkCreateShaderModule(device, &smci, nullptr, &shaderModule));

    // 8. Descriptor set layout (2 storage buffers)
    VkDescriptorSetLayoutBinding bindings[2]{};

    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo dsli{};
    dsli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsli.bindingCount = 2;
    dsli.pBindings = bindings;

    VkDescriptorSetLayout descSetLayout;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &dsli, nullptr, &descSetLayout));

    // 9. Pipeline layout
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &descSetLayout;

    VkPipelineLayout pipelineLayout;
    VK_CHECK(vkCreatePipelineLayout(device, &plci, nullptr, &pipelineLayout));

    // 10. Compute pipeline
    VkPipelineShaderStageCreateInfo ssci{};
    ssci.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ssci.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ssci.module = shaderModule;
    ssci.pName = "main";

    VkComputePipelineCreateInfo cpci2{};
    cpci2.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci2.stage = ssci;
    cpci2.layout = pipelineLayout;

    VkPipeline pipeline;
    VK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci2, nullptr, &pipeline));

    // 11. Descriptor pool + set
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = 2;

    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &poolSize;

    VkDescriptorPool descPool;
    VK_CHECK(vkCreateDescriptorPool(device, &dpci, nullptr, &descPool));

    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = descPool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &descSetLayout;

    VkDescriptorSet descSet;
    VK_CHECK(vkAllocateDescriptorSets(device, &dsai, &descSet));

    VkDescriptorBufferInfo srcInfo{};
    srcInfo.buffer = srcBuf;
    srcInfo.offset = 0;
    srcInfo.range = bufferSize;

    VkDescriptorBufferInfo dstInfo{};
    dstInfo.buffer = dstBuf;
    dstInfo.offset = 0;
    dstInfo.range = bufferSize;

    VkWriteDescriptorSet writes[2]{};

    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = descSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &srcInfo;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = descSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &dstInfo;

    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

    // 12. Command buffer
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;

    VkCommandBuffer cmdBuf;
    VK_CHECK(vkAllocateCommandBuffers(device, &cbai, &cmdBuf));

    VkCommandBufferBeginInfo cbbi{};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    VK_CHECK(vkBeginCommandBuffer(cmdBuf, &cbbi));

    vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipelineLayout, 0, 1, &descSet, 0, nullptr);

    // Dispatch 16 threads (matching local_size_x = 16)
    vkCmdDispatch(cmdBuf, 1, 1, 1);

    // Barrier so GPU writes are visible to CPU
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;

    vkCmdPipelineBarrier(cmdBuf,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT,
                         0,
                         1, &mb,
                         0, nullptr,
                         0, nullptr);

    VK_CHECK(vkEndCommandBuffer(cmdBuf));

    // 13. Submit and wait
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmdBuf;

    VkFence fence;
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VK_CHECK(vkCreateFence(device, &fci, nullptr, &fence));

    VK_CHECK(vkQueueSubmit(queue, 1, &si, fence));
    VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(1'000'000'000)));

    // 14. Read back dst buffer and print
    {
        void* p = nullptr;
        VK_CHECK(vkMapMemory(device, dstMem, 0, bufferSize, 0, &p));
        char* c = static_cast<char*>(p);
        std::cout << "GPU copied string: \"" << c << "\"\n";
        vkUnmapMemory(device, dstMem);
    }

    // 15. Cleanup
    vkDestroyFence(device, fence, nullptr);
    vkDestroyDescriptorPool(device, descPool, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    vkDestroyDescriptorSetLayout(device, descSetLayout, nullptr);
    vkDestroyShaderModule(device, shaderModule, nullptr);
    vkDestroyBuffer(device, srcBuf, nullptr);
    vkDestroyBuffer(device, dstBuf, nullptr);
    vkFreeMemory(device, srcMem, nullptr);
    vkFreeMemory(device, dstMem, nullptr);
    vkDestroyCommandPool(device, cmdPool, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);

    return 0;
}
