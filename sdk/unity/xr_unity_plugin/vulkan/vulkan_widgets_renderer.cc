/*
 * Copyright 2022 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "unity/xr_unity_plugin/vulkan/vulkan_widgets_renderer.h"

#include <array>
#include <cstdint>
#include <vector>

#include "rendering/android/vulkan/android_vulkan_loader.h"
#include "util/logging.h"
#include "unity/xr_unity_plugin/renderer.h"
#include "unity/xr_unity_plugin/vulkan/shaders/widget_frag.spv.h"
#include "unity/xr_unity_plugin/vulkan/shaders/widget_vert.spv.h"

// Vulkan call wrapper
#define CALL_VK(func)                                                    \
  {                                                                      \
    VkResult vkResult = (func);                                          \
    if (VK_SUCCESS != vkResult) {                                        \
      CARDBOARD_LOGE("Vulkan error. Error Code[%d], File[%s], line[%d]", \
                     vkResult, __FILE__, __LINE__);                      \
    }                                                                    \
  }

namespace cardboard::unity {

VulkanWidgetsRenderer::VulkanWidgetsRenderer(VkPhysicalDevice physical_device,
                                           VkDevice logical_device)
    : physical_device_(physical_device),
      logical_device_(logical_device),
      widget_image_count_(1),
      descriptor_set_layout_(VK_NULL_HANDLE),
      pipeline_layout_(VK_NULL_HANDLE),
      graphics_pipeline_(VK_NULL_HANDLE),
      vertex_buffers_(0),
      vertex_buffers_memory_(0),
      index_buffers_(VK_NULL_HANDLE),
      index_buffers_memory_(VK_NULL_HANDLE),
      descriptor_pool_(VK_NULL_HANDLE),
      descriptor_sets_(0),
      image_views_(0) {
  if (!rendering::LoadVulkan()) {
    CARDBOARD_LOGE("Failed to load vulkan lib in cardboard!");
    return;
  }

  CreateSharedVulkanObjects();
}

VulkanWidgetsRenderer::~VulkanWidgetsRenderer() {
  for (uint32_t i = 0; i < widget_image_count_; i++) {
    CleanTextureImageView(i);
  }

  rendering::vkDestroySampler(logical_device_, texture_sampler_, nullptr);
  rendering::vkDestroyPipelineLayout(logical_device_, pipeline_layout_,
                                     nullptr);
  rendering::vkDestroyDescriptorSetLayout(logical_device_,
                                          descriptor_set_layout_, nullptr);

  rendering::vkDestroyDescriptorPool(logical_device_, descriptor_pool_,
                                     nullptr);

  CleanPipeline();

  rendering::vkDestroyBuffer(logical_device_, index_buffers_, nullptr);
  rendering::vkFreeMemory(logical_device_, index_buffers_memory_, nullptr);

  for (uint32_t i = 0; i < widget_image_count_; i++) {
    if (vertex_buffers_[i] != VK_NULL_HANDLE) {
      rendering::vkDestroyBuffer(logical_device_, vertex_buffers_[i], nullptr);
      rendering::vkFreeMemory(logical_device_, vertex_buffers_memory_[i],
                              nullptr);
    }
  }
}

void VulkanWidgetsRenderer::RenderWidgets(
    const Renderer::ScreenParams& screen_params,
    const std::vector<Renderer::WidgetParams>& widgets_params,
    const VkCommandBuffer command_buffer, const VkRenderPass render_pass) {
  // TODO(241768790): Improve widgets renderer.
  widget_image_count_ = widgets_params.size();
  CreatePerWidgetVulkanObjects();
  UpdateVertexBuffers(widgets_params, screen_params);

  if (render_pass != current_render_pass_) {
    current_render_pass_ = render_pass;
    CreateGraphicsPipeline();
  }

  for (uint32_t i = 0; i < widget_image_count_; i++) {
    RenderWidget(widgets_params[i], command_buffer, i, screen_params);
  }
}

void VulkanWidgetsRenderer::CreateBuffer(VkDeviceSize size,
                                        VkBufferUsageFlags usage,
                                        VkMemoryPropertyFlags properties,
                                        VkBuffer& buffer,
                                        VkDeviceMemory& buffer_memory) {
  VkBufferCreateInfo buffer_info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                 .size = size,
                                 .usage = usage,
                                 .sharingMode = VK_SHARING_MODE_EXCLUSIVE};

  CALL_VK(rendering::vkCreateBuffer(logical_device_, &buffer_info, nullptr,
                                    &buffer));

  VkMemoryRequirements mem_requirements;
  rendering::vkGetBufferMemoryRequirements(logical_device_, buffer,
                                           &mem_requirements);

  VkMemoryAllocateInfo alloc_info{
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = mem_requirements.size,
      .memoryTypeIndex =
          FindMemoryType(mem_requirements.memoryTypeBits, properties)};

  CALL_VK(rendering::vkAllocateMemory(logical_device_, &alloc_info, nullptr,
                                      &buffer_memory));

  rendering::vkBindBufferMemory(logical_device_, buffer, buffer_memory, 0);
}

void VulkanWidgetsRenderer::CreateSharedVulkanObjects() {
  // Create DescriptorSet Layout
  VkDescriptorSetLayoutBinding bindings[1];

  VkDescriptorSetLayoutBinding sampler_layout_binding{
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      .pImmutableSamplers = nullptr,
  };
  bindings[0] = sampler_layout_binding;

  VkDescriptorSetLayoutCreateInfo layout_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = bindings,
  };
  CALL_VK(rendering::vkCreateDescriptorSetLayout(
      logical_device_, &layout_info, nullptr, &descriptor_set_layout_));

  // Create Pipeline Layout
  VkPipelineLayoutCreateInfo pipeline_layout_create_info{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .pNext = nullptr,
      .setLayoutCount = 1,
      .pSetLayouts = &descriptor_set_layout_,
      .pushConstantRangeCount = 0,
      .pPushConstantRanges = nullptr,
  };
  CALL_VK(rendering::vkCreatePipelineLayout(logical_device_,
                                            &pipeline_layout_create_info,
                                            nullptr, &pipeline_layout_));

  // Create Texture Sampler
  VkPhysicalDeviceProperties properties{};
  rendering::vkGetPhysicalDeviceProperties(physical_device_, &properties);

  VkSamplerCreateInfo sampler = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .pNext = nullptr,
      .magFilter = VK_FILTER_NEAREST,
      .minFilter = VK_FILTER_NEAREST,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
      .mipLodBias = 0.0f,
      .maxAnisotropy = properties.limits.maxSamplerAnisotropy,
      .compareOp = VK_COMPARE_OP_NEVER,
      .minLod = 0.0f,
      .maxLod = 0.0f,
      .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
      .unnormalizedCoordinates = VK_FALSE,
  };

  CALL_VK(rendering::vkCreateSampler(logical_device_, &sampler, nullptr,
                                     &texture_sampler_));

  // Create an index buffer to draw square textures.
  const std::vector<uint16_t> square_texture_indices = {0, 1, 2, 2, 3, 0};
  CreateIndexBuffer(square_texture_indices);
}

void VulkanWidgetsRenderer::CreatePerWidgetVulkanObjects() {
  // Create Descriptor Pool
  VkDescriptorPoolSize pool_sizes[1];
  pool_sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  pool_sizes[0].descriptorCount = static_cast<uint32_t>(widget_image_count_);

  VkDescriptorPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = pool_sizes;
  pool_info.maxSets = static_cast<uint32_t>(widget_image_count_);

  CALL_VK(rendering::vkCreateDescriptorPool(logical_device_, &pool_info,
                                            nullptr, &descriptor_pool_));

  // Create Descriptor Sets
  std::vector<VkDescriptorSetLayout> layouts(widget_image_count_,
                                             descriptor_set_layout_);
  VkDescriptorSetAllocateInfo alloc_info{};
  alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  alloc_info.descriptorPool = descriptor_pool_;
  alloc_info.descriptorSetCount = static_cast<uint32_t>(widget_image_count_);
  alloc_info.pSetLayouts = layouts.data();

  descriptor_sets_.resize(widget_image_count_);
  CALL_VK(rendering::vkAllocateDescriptorSets(logical_device_, &alloc_info,
                                              descriptor_sets_.data()));

  // Set the size of image view array to the amount of widgets.
  image_views_.resize(widget_image_count_);

  // Set the size of the vertex buffers array to the amount of widgets.
  vertex_buffers_.resize(widget_image_count_);
  vertex_buffers_memory_.resize(widget_image_count_);
}

void VulkanWidgetsRenderer::CreateGraphicsPipeline() {
  CleanPipeline();

  VkShaderModule vertex_shader =
      LoadShader(widget_vert, sizeof(widget_vert));
  VkShaderModule fragment_shader =
      LoadShader(widget_frag, sizeof(widget_frag));

  // Specify vertex and fragment shader stages
  VkPipelineShaderStageCreateInfo vertex_shader_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .stage = VK_SHADER_STAGE_VERTEX_BIT,
      .module = vertex_shader,
      .pName = "main",
      .pSpecializationInfo = nullptr,
  };
  VkPipelineShaderStageCreateInfo fragment_shader_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
      .module = fragment_shader,
      .pName = "main",
      .pSpecializationInfo = nullptr,
  };

  // Specify viewport info
  VkPipelineViewportStateCreateInfo viewport_info{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .pNext = nullptr,
      .viewportCount = 1,
      .pViewports = nullptr,
      .scissorCount = 1,
      .pScissors = nullptr,
  };

  // Specify multisample info
  VkSampleMask sample_mask = ~0u;
  VkPipelineMultisampleStateCreateInfo multisample_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .pNext = nullptr,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
      .sampleShadingEnable = VK_FALSE,
      .minSampleShading = 0,
      .pSampleMask = &sample_mask,
      .alphaToCoverageEnable = VK_FALSE,
      .alphaToOneEnable = VK_FALSE,
  };

  // Specify color blend state
  VkPipelineColorBlendAttachmentState attachment_states = {
      // .blendEnable = VK_FALSE,
      .blendEnable = VK_TRUE,
      .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
      .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .colorBlendOp = VK_BLEND_OP_ADD,
      .srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
      .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .alphaBlendOp = VK_BLEND_OP_ADD,
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
  };

  VkPipelineColorBlendStateCreateInfo color_blend_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .logicOpEnable = VK_FALSE,
      .logicOp = VK_LOGIC_OP_COPY,
      .attachmentCount = 1,
      .pAttachments = &attachment_states,
  };

  // Specify rasterizer info
  VkPipelineRasterizationStateCreateInfo raster_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .pNext = nullptr,
      .depthClampEnable = VK_FALSE,
      .rasterizerDiscardEnable = VK_FALSE,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_CLOCKWISE,
      .depthBiasEnable = VK_FALSE,
      .lineWidth = 1,
  };

  // Specify input assembler state
  VkPipelineInputAssemblyStateCreateInfo input_assembly_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .pNext = nullptr,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
      .primitiveRestartEnable = VK_FALSE,
  };

  // Specify vertex input state
  VkVertexInputBindingDescription vertex_input_bindings = {
      .binding = 0,
      .stride = 4 * sizeof(float),
      .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
  };

  VkVertexInputAttributeDescription vertex_input_attributes[2] = {
      {
          .location = 0,
          .binding = 0,
          .format = VK_FORMAT_R32G32_SFLOAT,
          .offset = 0,
      },
      {
          .location = 1,
          .binding = 0,
          .format = VK_FORMAT_R32G32_SFLOAT,
          .offset = sizeof(float) * 2,
      }};

  VkPipelineVertexInputStateCreateInfo vertex_input_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .pNext = nullptr,
      .vertexBindingDescriptionCount = 1,
      .pVertexBindingDescriptions = &vertex_input_bindings,
      .vertexAttributeDescriptionCount = 2,
      .pVertexAttributeDescriptions = vertex_input_attributes,
  };

  VkDynamicState dynamic_state_enables[2] = {
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
  };

  VkPipelineDynamicStateCreateInfo dynamic_state_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .pNext = nullptr,
      .dynamicStateCount = 2,
      .pDynamicStates = dynamic_state_enables};

  VkPipelineDepthStencilStateCreateInfo depth_stencil = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_TRUE,
      .depthWriteEnable = VK_TRUE,
      .depthCompareOp = VK_COMPARE_OP_LESS,
      .depthBoundsTestEnable = VK_FALSE,
      .stencilTestEnable = VK_FALSE};

  // Create the pipeline
  VkPipelineShaderStageCreateInfo shader_stages[2] = {vertex_shader_state,
                                                      fragment_shader_state};
  VkGraphicsPipelineCreateInfo pipeline_create_info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .stageCount = 2,
      .pStages = shader_stages,
      .pVertexInputState = &vertex_input_info,
      .pInputAssemblyState = &input_assembly_info,
      .pTessellationState = nullptr,
      .pViewportState = &viewport_info,
      .pRasterizationState = &raster_info,
      .pMultisampleState = &multisample_info,
      .pDepthStencilState = &depth_stencil,
      .pColorBlendState = &color_blend_info,
      .pDynamicState = &dynamic_state_info,
      .layout = pipeline_layout_,
      .renderPass = current_render_pass_,
      .subpass = 0,
      .basePipelineHandle = VK_NULL_HANDLE,
      .basePipelineIndex = 0,
  };
  CALL_VK(rendering::vkCreateGraphicsPipelines(logical_device_, VK_NULL_HANDLE,
                                               1, &pipeline_create_info,
                                               nullptr, &graphics_pipeline_));

  rendering::vkDestroyShaderModule(logical_device_, vertex_shader, nullptr);
  rendering::vkDestroyShaderModule(logical_device_, fragment_shader, nullptr);
}

VkShaderModule VulkanWidgetsRenderer::LoadShader(const uint32_t* const content,
                                                size_t size) const {
  VkShaderModule shader;
  VkShaderModuleCreateInfo shader_module_create_info{
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .codeSize = size,
      .pCode = content,
  };
  CALL_VK(rendering::vkCreateShaderModule(
      logical_device_, &shader_module_create_info, nullptr, &shader));

  return shader;
}

uint32_t VulkanWidgetsRenderer::FindMemoryType(
    uint32_t type_filter, VkMemoryPropertyFlags properties) {
  VkPhysicalDeviceMemoryProperties mem_properties;
  rendering::vkGetPhysicalDeviceMemoryProperties(physical_device_,
                                                 &mem_properties);

  for (uint32_t i = 0; i < mem_properties.memoryTypeCount; i++) {
    if ((type_filter & (1 << i)) &&
        (mem_properties.memoryTypes[i].propertyFlags & properties) ==
            properties) {
      return i;
    }
  }

  CARDBOARD_LOGE("Failed to find suitable memory type!");
  return 0;
}

void VulkanWidgetsRenderer::UpdateVertexBuffers(
    const std::vector<unity::Renderer::WidgetParams>& widgets_params,
    const unity::Renderer::ScreenParams& screen_params) {
  for (uint32_t i = 0; i < widget_image_count_; i++) {
    UpdateVertexBuffer(widgets_params[i], screen_params, i);
  }
}

void VulkanWidgetsRenderer::UpdateVertexBuffer(
    const unity::Renderer::WidgetParams& widget_params,
    const unity::Renderer::ScreenParams& screen_params, const uint32_t index) {
  if (vertex_buffers_[index] != VK_NULL_HANDLE) {
    rendering::vkDestroyBuffer(logical_device_, vertex_buffers_[index],
                               nullptr);
    rendering::vkFreeMemory(logical_device_, vertex_buffers_memory_[index],
                            nullptr);
  }

  // Convert coordinates to normalized space (-1,-1 - +1,+1)
  float x =
      Lerp(-1, +1,
           static_cast<float>(widget_params.x) / screen_params.viewport_width);
  // Translate the y coordinate of the widget from OpenGL coord system to Vulkan
  // coord system.
  // http://matthewwellings.com/blog/the-new-vulkan-coordinate-system/
  int opengl_to_vulkan_y =
      screen_params.viewport_height - widget_params.y - widget_params.height;
  float y = Lerp(
      -1, +1,
      static_cast<float>(opengl_to_vulkan_y) / screen_params.viewport_height);
  float width = widget_params.width * 2.0f / screen_params.viewport_width;
  float height = widget_params.height * 2.0f / screen_params.viewport_height;

  const std::vector<Vertex> vertices = {{x, y, 0.0f, 1.0f},
                                        {x, y + height, 0.0f, 0.0f},
                                        {x + width, y + height, 1.0f, 0.0f},
                                        {x + width, y, 1.0f, 1.0f}};

  // Create vertices for the widget.
  CreateVertexBuffer(vertices, index);
}

void VulkanWidgetsRenderer::RenderWidget(
    const unity::Renderer::WidgetParams& widget_params,
    VkCommandBuffer command_buffer, uint32_t image_index,
    const unity::Renderer::ScreenParams& screen_params) {
  // Update image and view
  VkImage* current_image = reinterpret_cast<VkImage*>(widget_params.texture);
  CleanTextureImageView(image_index);
  const VkImageViewCreateInfo view_create_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .image = *current_image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      // This is the format set by Unity
      .format = VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK,
      .components =
          {
              .r = VK_COMPONENT_SWIZZLE_R,
              .g = VK_COMPONENT_SWIZZLE_G,
              .b = VK_COMPONENT_SWIZZLE_B,
              .a = VK_COMPONENT_SWIZZLE_A,
          },
      .subresourceRange =
          {
              .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
              .baseMipLevel = 0,
              .levelCount = 1,
              .baseArrayLayer = 0,
              .layerCount = 1,
          },
  };
  CALL_VK(rendering::vkCreateImageView(logical_device_, &view_create_info,
                                       nullptr /* pAllocator */,
                                       &image_views_[image_index]));

  // Update Descriptor Sets
  VkDescriptorImageInfo image_info{
      .sampler = texture_sampler_,
      .imageView = image_views_[image_index],
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };

  VkWriteDescriptorSet descriptor_writes[1];

  descriptor_writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  descriptor_writes[0].dstSet = descriptor_sets_[image_index];
  descriptor_writes[0].dstBinding = 0;
  descriptor_writes[0].dstArrayElement = 0;
  descriptor_writes[0].descriptorType =
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  descriptor_writes[0].descriptorCount = 1;
  descriptor_writes[0].pImageInfo = &image_info;
  descriptor_writes[0].pNext = nullptr;

  rendering::vkUpdateDescriptorSets(logical_device_, 1, descriptor_writes, 0,
                                    nullptr);

  // Update Viewport and scissor
  VkViewport viewport = {
      .x = static_cast<float>(screen_params.viewport_x),
      .y = static_cast<float>(screen_params.viewport_y),
      .width = static_cast<float>(screen_params.viewport_width),
      .height = static_cast<float>(screen_params.viewport_height),
      .minDepth = 0.0,
      .maxDepth = 1.0};

  VkRect2D scissor = {
      .extent = {.width = static_cast<uint32_t>(screen_params.viewport_width),
                 .height =
                     static_cast<uint32_t>(screen_params.viewport_height)},
  };

  scissor.offset = {.x = screen_params.viewport_x,
                    .y = screen_params.viewport_y};

  // Bind to the command buffer.
  rendering::vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                               graphics_pipeline_);
  rendering::vkCmdSetViewport(command_buffer, 0, 1, &viewport);
  rendering::vkCmdSetScissor(command_buffer, 0, 1, &scissor);

  VkDeviceSize offset = 0;
  rendering::vkCmdBindVertexBuffers(command_buffer, 0, 1,
                                    &vertex_buffers_[image_index], &offset);

  rendering::vkCmdBindIndexBuffer(command_buffer, index_buffers_, 0,
                                  VK_INDEX_TYPE_UINT16);

  rendering::vkCmdBindDescriptorSets(
      command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1,
      &descriptor_sets_[image_index], 0, nullptr);
  rendering::vkCmdDrawIndexed(
      command_buffer, static_cast<uint32_t>(indices_count_), 1, 0, 0, 0);
}

void VulkanWidgetsRenderer::CleanPipeline() {
  if (graphics_pipeline_ != VK_NULL_HANDLE) {
    rendering::vkDestroyPipeline(logical_device_, graphics_pipeline_, nullptr);
    graphics_pipeline_ = VK_NULL_HANDLE;
  }
}

void VulkanWidgetsRenderer::CleanTextureImageView(int index) {
  if (image_views_[index] != VK_NULL_HANDLE) {
    rendering::vkDestroyImageView(logical_device_, image_views_[index],
                                  nullptr /* vkDestroyImageView */);
    image_views_[index] = VK_NULL_HANDLE;
  }
}

void VulkanWidgetsRenderer::CreateVertexBuffer(std::vector<Vertex> vertices,
                                              const uint32_t index) {
  if (index >= vertex_buffers_.size()) {
    CARDBOARD_LOGE("Index is bigger than the buffers vector size.");
    return;
  }

  VkDeviceSize buffer_size = sizeof(vertices[0]) * vertices.size();
  CreateBuffer(buffer_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
               vertex_buffers_[index], vertex_buffers_memory_[index]);

  void* data;
  CALL_VK(rendering::vkMapMemory(logical_device_, vertex_buffers_memory_[index],
                                 0, buffer_size, 0, &data));
  memcpy(data, vertices.data(), buffer_size);
  rendering::vkUnmapMemory(logical_device_, vertex_buffers_memory_[index]);
}

void VulkanWidgetsRenderer::CreateIndexBuffer(std::vector<uint16_t> indices) {
  VkDeviceSize buffer_size = sizeof(indices[0]) * indices.size();
  CreateBuffer(buffer_size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
               index_buffers_, index_buffers_memory_);

  void* data;
  rendering::vkMapMemory(logical_device_, index_buffers_memory_, 0, buffer_size,
                         0, &data);
  memcpy(data, indices.data(), buffer_size);
  rendering::vkUnmapMemory(logical_device_, index_buffers_memory_);

  indices_count_ = indices.size();
}

}  // namespace cardboard::unity
