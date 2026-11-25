#pragma once
#include <cstdint>
#include <cstddef>
#define VK_NULL_HANDLE nullptr
#define VK_WHOLE_SIZE (~uint64_t(0))
#define VK_SUCCESS 0
#define VK_ERROR_OUT_OF_HOST_MEMORY -1
#define VK_ERROR_OUT_OF_DEVICE_MEMORY -2
#define VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO 1
#define VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO 2
#define VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE 3
#define VK_STRUCTURE_TYPE_MEMORY_BARRIER 4
#define VK_BUFFER_USAGE_STORAGE_BUFFER_BIT 1
#define VK_BUFFER_USAGE_TRANSFER_DST_BIT 2
#define VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT 4
#define VK_BUFFER_USAGE_TRANSFER_SRC_BIT 8
#define VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT 1
#define VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT 2
#define VK_MEMORY_PROPERTY_HOST_COHERENT_BIT 4
#define VK_SHARING_MODE_EXCLUSIVE 0
#define VK_ACCESS_SHADER_READ_BIT 1
#define VK_ACCESS_SHADER_WRITE_BIT 2
#define VK_ACCESS_INDIRECT_COMMAND_READ_BIT 4
#define VK_ACCESS_TRANSFER_WRITE_BIT 8
#define VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT 1
#define VK_PIPELINE_STAGE_VERTEX_SHADER_BIT 2
#define VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT 4
#define VK_PIPELINE_STAGE_TRANSFER_BIT 8
#define VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT 16
#define VK_PIPELINE_STAGE_HOST_BIT 64
#define VK_ACCESS_HOST_READ_BIT 32
using VkDeviceSize=uint64_t;
using VkResult=int;
using VkBufferUsageFlags=uint32_t;
using VkMemoryPropertyFlags=uint32_t;
using VkPipelineStageFlags=uint32_t;
using VkAccessFlags=uint32_t;
using VkDevice=void*;
using VkPhysicalDevice=void*;
using VkBuffer=void*;
using VkDeviceMemory=void*;
using VkCommandBuffer=void*;
using VkDescriptorSetLayout=void*;
using VkDescriptorSet=void*;
using VkDescriptorPool=void*;
using VkPipeline=void*;
using VkPipelineLayout=void*;
using VkBufferCreateFlags=uint32_t;
using VkSharingMode=uint32_t;
struct VkBufferCreateInfo {int sType; void* pNext=nullptr; VkBufferCreateFlags flags=0; VkDeviceSize size=0; VkBufferUsageFlags usage=0; VkSharingMode sharingMode=0;};
struct VkMemoryRequirements {VkDeviceSize size=0,alignment=0;uint32_t memoryTypeBits=0;};
struct VkMemoryType {VkMemoryPropertyFlags propertyFlags=0;uint32_t heapIndex=0;};
struct VkPhysicalDeviceMemoryProperties {uint32_t memoryTypeCount=0;VkMemoryType memoryTypes[32]{};};
struct VkMemoryAllocateInfo {int sType;void* pNext=nullptr;VkDeviceSize allocationSize=0;uint32_t memoryTypeIndex=0;};
struct VkMappedMemoryRange {int sType;void* pNext=nullptr;VkDeviceMemory memory=nullptr;VkDeviceSize offset=0,size=0;};
struct VkPhysicalDeviceProperties {char pad[1000]{};struct {VkDeviceSize nonCoherentAtomSize=1;uint32_t maxStorageBufferRange=128*1024*1024;uint32_t maxPerStageDescriptorStorageBuffers=16,maxDescriptorSetStorageBuffers=16,maxComputeWorkGroupSize[3]{1024,1024,64},maxComputeWorkGroupCount[3]{65535,65535,65535},maxComputeWorkGroupInvocations=1024,maxPushConstantsSize=128,maxComputeSharedMemorySize=32768;} limits;};
struct VkBufferCopy {VkDeviceSize srcOffset=0,dstOffset=0,size=0;};
struct VkMemoryBarrier {int sType;void* pNext=nullptr;VkAccessFlags srcAccessMask=0,dstAccessMask=0;};
struct VkDrawIndirectCommand {uint32_t vertexCount,instanceCount,firstVertex,firstInstance;};
extern "C" {
VkResult vkCreateBuffer(VkDevice,const VkBufferCreateInfo*,const void*,VkBuffer*);
void vkGetBufferMemoryRequirements(VkDevice,VkBuffer,VkMemoryRequirements*);
void vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice,VkPhysicalDeviceMemoryProperties*);
void vkGetPhysicalDeviceProperties(VkPhysicalDevice,VkPhysicalDeviceProperties*);
VkResult vkAllocateMemory(VkDevice,const VkMemoryAllocateInfo*,const void*,VkDeviceMemory*);
VkResult vkBindBufferMemory(VkDevice,VkBuffer,VkDeviceMemory,VkDeviceSize);
VkResult vkMapMemory(VkDevice,VkDeviceMemory,VkDeviceSize,VkDeviceSize,uint32_t,void**);
VkResult vkFlushMappedMemoryRanges(VkDevice,uint32_t,const VkMappedMemoryRange*);
VkResult vkInvalidateMappedMemoryRanges(VkDevice,uint32_t,const VkMappedMemoryRange*);
void vkUnmapMemory(VkDevice,VkDeviceMemory);
void vkDestroyBuffer(VkDevice,VkBuffer,const void*);
void vkFreeMemory(VkDevice,VkDeviceMemory,const void*);
void vkCmdPipelineBarrier(VkCommandBuffer,VkPipelineStageFlags,VkPipelineStageFlags,uint32_t,uint32_t,const VkMemoryBarrier*,uint32_t,const void*,uint32_t,const void*);
void vkCmdFillBuffer(VkCommandBuffer,VkBuffer,VkDeviceSize,VkDeviceSize,uint32_t);
void vkCmdCopyBuffer(VkCommandBuffer,VkBuffer,VkBuffer,uint32_t,const VkBufferCopy*);
void vkCmdUpdateBuffer(VkCommandBuffer,VkBuffer,VkDeviceSize,VkDeviceSize,const void*);
}
// Minimal types for VulkanBlockAtlas *host unit tests only*.
#define VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO 11
#define VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO 12
#define VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO 13
#define VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER 14
#define VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO 15
#define VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO 16
#define VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET 17
#define VK_FORMAT_R8G8B8A8_SRGB 43
#define VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT 1
#define VK_FORMAT_FEATURE_BLIT_SRC_BIT 2
#define VK_FORMAT_FEATURE_BLIT_DST_BIT 4
#define VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT 8
#define VK_IMAGE_TYPE_2D 1
#define VK_IMAGE_VIEW_TYPE_2D 1
#define VK_SAMPLE_COUNT_1_BIT 1
#define VK_IMAGE_TILING_OPTIMAL 0
#define VK_IMAGE_USAGE_TRANSFER_SRC_BIT 1
#define VK_IMAGE_USAGE_TRANSFER_DST_BIT 2
#define VK_IMAGE_USAGE_SAMPLED_BIT 4
#define VK_IMAGE_LAYOUT_UNDEFINED 0
#define VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL 1
#define VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL 2
#define VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL 3
#define VK_IMAGE_ASPECT_COLOR_BIT 1
#define VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT 32
#define VK_ACCESS_TRANSFER_READ_BIT 16
#define VK_QUEUE_FAMILY_IGNORED 0xffffffffu
#define VK_FILTER_NEAREST 0
#define VK_FILTER_LINEAR 1
#define VK_SAMPLER_MIPMAP_MODE_NEAREST 0
#define VK_SAMPLER_ADDRESS_MODE_REPEAT 0
#define VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE 2
#define VK_BORDER_COLOR_INT_OPAQUE_BLACK 3
#define VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER 1
using VkFormat=int;
using VkFormatFeatureFlags=uint32_t;
using VkImage=void*;
using VkImageView=void*;
using VkSampler=void*;
using VkImageLayout=int;
using VkImageAspectFlags=uint32_t;
using VkImageUsageFlags=uint32_t;
struct VkExtent3D{uint32_t width=0,height=0,depth=0;};
struct VkOffset3D{int32_t x=0,y=0,z=0;};
struct VkFormatProperties{VkFormatFeatureFlags linearTilingFeatures=0, optimalTilingFeatures=0, bufferFeatures=0;};
struct VkImageCreateInfo{int sType;void* pNext=nullptr;uint32_t flags=0;uint32_t imageType=0;VkFormat format=0;VkExtent3D extent{};uint32_t mipLevels=0,arrayLayers=0,samples=0,tiling=0;VkImageUsageFlags usage=0;VkSharingMode sharingMode=0;VkImageLayout initialLayout=0;};
struct VkImageSubresourceRange{VkImageAspectFlags aspectMask=0;uint32_t baseMipLevel=0,levelCount=0,baseArrayLayer=0,layerCount=0;};
struct VkImageViewCreateInfo{int sType;void* pNext=nullptr;uint32_t flags=0;VkImage image=nullptr;uint32_t viewType=0;VkFormat format=0;uint32_t components=0;VkImageSubresourceRange subresourceRange{};};
struct VkSamplerCreateInfo{int sType;void* pNext=nullptr;uint32_t flags=0,magFilter=0,minFilter=0,mipmapMode=0,addressModeU=0,addressModeV=0,addressModeW=0;float mipLodBias=0;uint32_t anisotropyEnable=0;float maxAnisotropy=0;uint32_t compareEnable=0,compareOp=0;float minLod=0,maxLod=0;uint32_t borderColor=0,unnormalizedCoordinates=0;};
struct VkImageMemoryBarrier{int sType;void* pNext=nullptr;VkAccessFlags srcAccessMask=0,dstAccessMask=0;VkImageLayout oldLayout=0,newLayout=0;uint32_t srcQueueFamilyIndex=0,dstQueueFamilyIndex=0;VkImage image=nullptr;VkImageSubresourceRange subresourceRange{};};
struct VkImageSubresourceLayers{VkImageAspectFlags aspectMask=0;uint32_t mipLevel=0,baseArrayLayer=0,layerCount=0;};
struct VkBufferImageCopy{VkDeviceSize bufferOffset=0;uint32_t bufferRowLength=0,bufferImageHeight=0;VkImageSubresourceLayers imageSubresource{};VkOffset3D imageOffset{};VkExtent3D imageExtent{};};
struct VkImageBlit{VkImageSubresourceLayers srcSubresource{};VkOffset3D srcOffsets[2]{};VkImageSubresourceLayers dstSubresource{};VkOffset3D dstOffsets[2]{};};
struct VkDescriptorPoolSize{uint32_t type=0,descriptorCount=0;};
struct VkDescriptorPoolCreateInfo{int sType;void* pNext=nullptr;uint32_t flags=0,maxSets=0,poolSizeCount=0;const VkDescriptorPoolSize* pPoolSizes=nullptr;};
struct VkDescriptorSetAllocateInfo{int sType;void* pNext=nullptr;VkDescriptorPool descriptorPool=nullptr;uint32_t descriptorSetCount=0;const VkDescriptorSetLayout* pSetLayouts=nullptr;};
struct VkDescriptorImageInfo{VkSampler sampler=nullptr;VkImageView imageView=nullptr;VkImageLayout imageLayout=0;};
struct VkWriteDescriptorSet{int sType;void* pNext=nullptr;VkDescriptorSet dstSet=nullptr;uint32_t dstBinding=0,dstArrayElement=0,descriptorCount=0,descriptorType=0;const VkDescriptorImageInfo* pImageInfo=nullptr;const void* pBufferInfo=nullptr;const void* pTexelBufferView=nullptr;};
extern "C" {
void vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice,VkFormat,VkFormatProperties*);
VkResult vkCreateImage(VkDevice,const VkImageCreateInfo*,const void*,VkImage*);
void vkGetImageMemoryRequirements(VkDevice,VkImage,VkMemoryRequirements*);
VkResult vkBindImageMemory(VkDevice,VkImage,VkDeviceMemory,VkDeviceSize);
VkResult vkCreateImageView(VkDevice,const VkImageViewCreateInfo*,const void*,VkImageView*);
VkResult vkCreateSampler(VkDevice,const VkSamplerCreateInfo*,const void*,VkSampler*);
void vkDestroyImage(VkDevice,VkImage,const void*);
void vkDestroyImageView(VkDevice,VkImageView,const void*);
void vkDestroySampler(VkDevice,VkSampler,const void*);
void vkCmdCopyBufferToImage(VkCommandBuffer,VkBuffer,VkImage,VkImageLayout,uint32_t,const VkBufferImageCopy*);
void vkCmdBlitImage(VkCommandBuffer,VkImage,VkImageLayout,VkImage,VkImageLayout,uint32_t,const VkImageBlit*,uint32_t);
VkResult vkCreateDescriptorPool(VkDevice,const VkDescriptorPoolCreateInfo*,const void*,VkDescriptorPool*);
VkResult vkAllocateDescriptorSets(VkDevice,const VkDescriptorSetAllocateInfo*,VkDescriptorSet*);
void vkUpdateDescriptorSets(VkDevice,uint32_t,const VkWriteDescriptorSet*,uint32_t,const void*);
void vkDestroyDescriptorPool(VkDevice,VkDescriptorPool,const void*);
}

// Minimal additional declarations for syntax checks of the Hi-Z compute owner.
#define VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO 18
#define VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO 19
#define VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO 20
#define VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO 21
#define VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO 22
#define VK_DESCRIPTOR_TYPE_STORAGE_BUFFER 2
#define VK_SHADER_STAGE_COMPUTE_BIT 1
#define VK_SHADER_STAGE_VERTEX_BIT 2
#define VK_SHADER_STAGE_FRAGMENT_BIT 4
#define VK_PIPELINE_BIND_POINT_COMPUTE 1
#define VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT 128
#define VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT 256
#define VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT 64
#define VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT 128
#define VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL 4
#define VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL 5
#define VK_IMAGE_ASPECT_DEPTH_BIT 2
#define VK_FORMAT_UNDEFINED 0
using VkInstance=void*;using VkSurfaceKHR=void*;using VkDebugUtilsMessengerEXT=void*;
using VkRenderPass=void*;using VkFramebuffer=void*;using VkQueue=void*;
using VkSwapchainKHR=void*;using VkFence=void*;using VkSemaphore=void*;using VkCommandPool=void*;
using VkShaderModule=void*;
struct VkExtent2D{uint32_t width=0,height=0;};
struct VkDescriptorSetLayoutBinding{uint32_t binding=0,descriptorType=0,descriptorCount=0,stageFlags=0;const VkSampler* pImmutableSamplers=nullptr;};
struct VkDescriptorSetLayoutCreateInfo{int sType;void* pNext=nullptr;uint32_t flags=0,bindingCount=0;const VkDescriptorSetLayoutBinding* pBindings=nullptr;};
struct VkDescriptorBufferInfo{VkBuffer buffer=nullptr;VkDeviceSize offset=0,range=0;};
struct VkPushConstantRange{uint32_t stageFlags=0,offset=0,size=0;};
struct VkPipelineLayoutCreateInfo{int sType;void* pNext=nullptr;uint32_t flags=0,setLayoutCount=0;const VkDescriptorSetLayout* pSetLayouts=nullptr;uint32_t pushConstantRangeCount=0;const VkPushConstantRange* pPushConstantRanges=nullptr;};
struct VkShaderModuleCreateInfo{int sType;void* pNext=nullptr;uint32_t flags=0;size_t codeSize=0;const uint32_t* pCode=nullptr;};
struct VkPipelineShaderStageCreateInfo{int sType;void* pNext=nullptr;uint32_t flags=0,stage=0;VkShaderModule module=nullptr;const char* pName=nullptr;const void* pSpecializationInfo=nullptr;};
struct VkComputePipelineCreateInfo{int sType;void* pNext=nullptr;uint32_t flags=0;VkPipelineShaderStageCreateInfo stage{};VkPipelineLayout layout=nullptr;VkPipeline basePipelineHandle=nullptr;int32_t basePipelineIndex=0;};
extern "C" {
VkResult vkCreateDescriptorSetLayout(VkDevice,const VkDescriptorSetLayoutCreateInfo*,const void*,VkDescriptorSetLayout*);
VkResult vkCreatePipelineLayout(VkDevice,const VkPipelineLayoutCreateInfo*,const void*,VkPipelineLayout*);
VkResult vkCreateShaderModule(VkDevice,const VkShaderModuleCreateInfo*,const void*,VkShaderModule*);
VkResult vkCreateComputePipelines(VkDevice,void*,uint32_t,const VkComputePipelineCreateInfo*,const void*,VkPipeline*);
void vkDestroyShaderModule(VkDevice,VkShaderModule,const void*);
void vkDestroyPipeline(VkDevice,VkPipeline,const void*);
void vkDestroyPipelineLayout(VkDevice,VkPipelineLayout,const void*);
void vkDestroyDescriptorSetLayout(VkDevice,VkDescriptorSetLayout,const void*);
void vkCmdBindPipeline(VkCommandBuffer,uint32_t,VkPipeline);
void vkCmdBindDescriptorSets(VkCommandBuffer,uint32_t,VkPipelineLayout,uint32_t,uint32_t,const VkDescriptorSet*,uint32_t,const uint32_t*);
void vkCmdPushConstants(VkCommandBuffer,VkPipelineLayout,uint32_t,uint32_t,uint32_t,const void*);
void vkCmdDispatch(VkCommandBuffer,uint32_t,uint32_t,uint32_t);
void vkCmdDispatchIndirect(VkCommandBuffer,VkBuffer,VkDeviceSize);
}

// Graphics pipeline declarations used only by the host-only Stage24 mock.
#define VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO 40
#define VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO 41
#define VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO 42
#define VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO 43
#define VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO 44
#define VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO 45
#define VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO 46
#define VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO 47
#define VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO 48
#define VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST 3
#define VK_DYNAMIC_STATE_VIEWPORT 0
#define VK_DYNAMIC_STATE_SCISSOR 1
#define VK_POLYGON_MODE_FILL 0
#define VK_CULL_MODE_NONE 0
#define VK_CULL_MODE_BACK_BIT 2
#define VK_FRONT_FACE_COUNTER_CLOCKWISE 0
#define VK_COLOR_COMPONENT_R_BIT 1
#define VK_COLOR_COMPONENT_G_BIT 2
#define VK_COLOR_COMPONENT_B_BIT 4
#define VK_COLOR_COMPONENT_A_BIT 8
#define VK_TRUE 1
#define VK_COMPARE_OP_LESS_OR_EQUAL 3
using VkDynamicState=uint32_t;
struct VkPipelineVertexInputStateCreateInfo{int sType;};
struct VkPipelineInputAssemblyStateCreateInfo{int sType;uint32_t topology=0;};
struct VkPipelineViewportStateCreateInfo{int sType;uint32_t viewportCount=0,scissorCount=0;};
struct VkPipelineDynamicStateCreateInfo{int sType;uint32_t dynamicStateCount=0;const VkDynamicState* pDynamicStates=nullptr;};
struct VkPipelineRasterizationStateCreateInfo{int sType;uint32_t polygonMode=0,cullMode=0,frontFace=0;float lineWidth=0;};
struct VkPipelineMultisampleStateCreateInfo{int sType;uint32_t rasterizationSamples=0;};
struct VkPipelineColorBlendAttachmentState{uint32_t colorWriteMask=0;};
struct VkPipelineColorBlendStateCreateInfo{int sType;uint32_t attachmentCount=0;const VkPipelineColorBlendAttachmentState* pAttachments=nullptr;};
struct VkPipelineDepthStencilStateCreateInfo{int sType;uint32_t depthTestEnable=0,depthWriteEnable=0,depthCompareOp=0;};
struct VkGraphicsPipelineCreateInfo{
    int sType;uint32_t stageCount=0;const VkPipelineShaderStageCreateInfo* pStages=nullptr;
    const VkPipelineVertexInputStateCreateInfo* pVertexInputState=nullptr;
    const VkPipelineInputAssemblyStateCreateInfo* pInputAssemblyState=nullptr;
    const VkPipelineViewportStateCreateInfo* pViewportState=nullptr;
    const VkPipelineRasterizationStateCreateInfo* pRasterizationState=nullptr;
    const VkPipelineMultisampleStateCreateInfo* pMultisampleState=nullptr;
    const VkPipelineDepthStencilStateCreateInfo* pDepthStencilState=nullptr;
    const VkPipelineColorBlendStateCreateInfo* pColorBlendState=nullptr;
    const VkPipelineDynamicStateCreateInfo* pDynamicState=nullptr;
    VkPipelineLayout layout=nullptr;VkRenderPass renderPass=nullptr;
};
extern "C" VkResult vkCreateGraphicsPipelines(VkDevice,void*,uint32_t,const VkGraphicsPipelineCreateInfo*,const void*,VkPipeline*);
