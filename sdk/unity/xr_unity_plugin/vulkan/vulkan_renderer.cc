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
#include <array>
#include <memory>
#include <vector>

#include "include/cardboard.h"
#include "rendering/android/vulkan/android_vulkan_loader.h"
#include "util/is_arg_null.h"
#include "util/logging.h"
#include "unity/xr_unity_plugin/renderer.h"
#include "unity/xr_unity_plugin/vulkan/vulkan_widgets_renderer.h"
#include "IUnityRenderingExtensions.h"
#include "IUnityGraphicsVulkan.h"

namespace cardboard::unity {
namespace {

PFN_vkGetInstanceProcAddr Orig_GetInstanceProcAddr;
PFN_vkCreateSwapchainKHR Orig_vkCreateSwapchainKHR;
PFN_vkAcquireNextImageKHR Orig_vkAcquireNextImageKHR;
VkSwapchainKHR cached_swapchain;
uint32_t image_index;

/**
 * Function registerd to intercept the vulkan function `vkCreateSwapchainKHR`.
 * Through this function we could get the swapchain that Unity created.
 */
static VKAPI_ATTR void VKAPI_CALL Hook_vkCreateSwapchainKHR(
    VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain) {
  cardboard::rendering::vkCreateSwapchainKHR(device, pCreateInfo, pAllocator,
                                             pSwapchain);
  cached_swapchain = *pSwapchain;
}

/**
 * Function registerd to intercept the vulkan function `vkAcquireNextImageKHR`.
 * Through this function we could get the image index in the swapchain for
 * each frame.
 */
static VKAPI_ATTR void VKAPI_CALL Hook_vkAcquireNextImageKHR(
    VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, uint32_t* pImageIndex) {
  cardboard::rendering::vkAcquireNextImageKHR(device, swapchain, timeout,
                                              semaphore, fence, pImageIndex);
  image_index = *pImageIndex;
}

/**
 * Function used to register the Vulkan interception functions.
 */
PFN_vkVoidFunction VKAPI_PTR MyGetInstanceProcAddr(VkInstance instance,
                                                   const char* pName) {
  if (strcmp(pName, "vkCreateSwapchainKHR") == 0) {
    Orig_vkCreateSwapchainKHR =
        (PFN_vkCreateSwapchainKHR)Orig_GetInstanceProcAddr(instance, pName);
    return (PFN_vkVoidFunction)&Hook_vkCreateSwapchainKHR;
  }

  if (strcmp(pName, "vkAcquireNextImageKHR") == 0) {
    Orig_vkAcquireNextImageKHR =
        (PFN_vkAcquireNextImageKHR)Orig_GetInstanceProcAddr(instance, pName);
    return (PFN_vkVoidFunction)&Hook_vkAcquireNextImageKHR;
  }

  return Orig_GetInstanceProcAddr(instance, pName);
}

/**
 * Register the interception function during the initialization.
 */
PFN_vkGetInstanceProcAddr InterceptVulkanInitialization(
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr, void* /*userdata*/) {
  Orig_GetInstanceProcAddr = GetInstanceProcAddr;
  return &MyGetInstanceProcAddr;
}

/**
 * This function is exported so the plugin could call it during loading.
 */
extern "C" void RenderAPI_Vulkan_OnPluginLoad(IUnityInterfaces* interfaces) {
  IUnityGraphicsVulkanV2* vulkan_interface =
      interfaces->Get<IUnityGraphicsVulkanV2>();

  vulkan_interface->AddInterceptInitialization(InterceptVulkanInitialization,
                                               NULL, 2);
  cardboard::rendering::LoadVulkan();
}

class VulkanRenderer : public Renderer {
 public:
  explicit VulkanRenderer(IUnityInterfaces* xr_interfaces)
      : current_rendering_width_(0), current_rendering_height_(0) {
    vulkan_interface_ = xr_interfaces->Get<IUnityGraphicsVulkanV2>();
    if (CARDBOARD_IS_ARG_NULL(vulkan_interface_)) {
      return;
    }

    UnityVulkanInstance vulkanInstance = vulkan_interface_->Instance();
    logical_device_ = vulkanInstance.device;
    physical_device_ = vulkanInstance.physicalDevice;

    cardboard::rendering::vkGetSwapchainImagesKHR(
        logical_device_, cached_swapchain, &swapchain_image_count_, nullptr);
    swapchain_images_.resize(swapchain_image_count_);
    swapchain_views_.resize(swapchain_image_count_);
    frame_buffers_.resize(swapchain_image_count_);

    // Get the images from the swapchain and wrap it into a image view.
    cardboard::rendering::vkGetSwapchainImagesKHR(
        logical_device_, cached_swapchain, &swapchain_image_count_,
        swapchain_images_.data());

    for (size_t i = 0; i < swapchain_images_.size(); i++) {
      const VkImageViewCreateInfo view_create_info = {
          .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
          .pNext = nullptr,
          .flags = 0,
          .image = swapchain_images_[i],
          .viewType = VK_IMAGE_VIEW_TYPE_2D,
          .format = VK_FORMAT_R8G8B8A8_SRGB,
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

      cardboard::rendering::vkCreateImageView(
          logical_device_, &view_create_info, nullptr /* pAllocator */,
          &swapchain_views_[i]);
    }

    // Create RenderPass
    const VkAttachmentDescription attachment_descriptions{
        .format = VK_FORMAT_R8G8B8A8_SRGB,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };

    const VkAttachmentReference colour_reference = {
        .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

    const VkSubpassDescription subpass_description{
        .flags = 0,
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .inputAttachmentCount = 0,
        .pInputAttachments = nullptr,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colour_reference,
        .pResolveAttachments = nullptr,
        .pDepthStencilAttachment = nullptr,
        .preserveAttachmentCount = 0,
        .pPreserveAttachments = nullptr,
    };
    const VkRenderPassCreateInfo render_pass_create_info{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .pNext = nullptr,
        .attachmentCount = 1,
        .pAttachments = &attachment_descriptions,
        .subpassCount = 1,
        .pSubpasses = &subpass_description,
        .dependencyCount = 0,
        .pDependencies = nullptr,
    };
    cardboard::rendering::vkCreateRenderPass(
        logical_device_, &render_pass_create_info, nullptr /* pAllocator */,
        &render_pass_);
  }

  ~VulkanRenderer() {
    TeardownWidgets();

    // Remove the Vulkan resources created by this VulkanRenderer.
    for (uint32_t i = 0; i < swapchain_image_count_; i++) {
      cardboard::rendering::vkDestroyFramebuffer(
          logical_device_, frame_buffers_[i], nullptr /* pAllocator */);
      cardboard::rendering::vkDestroyImageView(
          logical_device_, swapchain_views_[i],
          nullptr /* vkDestroyImageView */);
    }

    cardboard::rendering::vkDestroyRenderPass(logical_device_, render_pass_,
                                              nullptr);
  }

  void SetupWidgets() override {
    widget_renderer_ = std::make_unique<VulkanWidgetsRenderer>(physical_device_,
                                                              logical_device_);
  }

  void RenderWidgets(const ScreenParams& screen_params,
                     const std::vector<WidgetParams>& widget_params) override {
    widget_renderer_->RenderWidgets(screen_params, widget_params,
                                    current_command_buffer_, render_pass_);
  }

  void TeardownWidgets() override {
    if (widget_renderer_ != nullptr) {
      widget_renderer_.reset(nullptr);
    }
  }

  void CreateRenderTexture(RenderTexture* render_texture, int screen_width,
                           int screen_height) override {
    VkImageCreateInfo imageInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_SRGB,
        .extent =
            {
                .width = static_cast<uint32_t>(screen_width / 2),
                .height = static_cast<uint32_t>(screen_height),
                .depth = 1,
            },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    VkImage image;
    cardboard::rendering::vkCreateImage(logical_device_, &imageInfo, nullptr,
                                        &image);

    VkMemoryRequirements memRequirements;
    cardboard::rendering::vkGetImageMemoryRequirements(logical_device_, image,
                                                       &memRequirements);

    VkMemoryAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memRequirements.size,
        .memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };

    VkDeviceMemory texture_image_memory;
    cardboard::rendering::vkAllocateMemory(logical_device_, &allocInfo, nullptr,
                                           &texture_image_memory);
    cardboard::rendering::vkBindImageMemory(logical_device_, image,
                                            texture_image_memory, 0);

    // Unity requires an VkImage in order to draw the scene.
    render_texture->color_buffer = reinterpret_cast<uint64_t>(image);

    // When using Vulkan, texture depth buffer is unused.
    render_texture->depth_buffer = 0;
  }

  void DestroyRenderTexture(RenderTexture* render_texture) override {
    render_texture->color_buffer = 0;
    render_texture->depth_buffer = 0;
  }

  void RenderEyesToDisplay(
      CardboardDistortionRenderer* renderer, const ScreenParams& screen_params,
      const CardboardEyeTextureDescription* left_eye,
      const CardboardEyeTextureDescription* right_eye) override {
    // Setup rendering content
    CardboardVulkanDistortionRendererTarget target_config{
        .vk_render_pass = reinterpret_cast<uint64_t>(&render_pass_),
        .vk_command_buffer =
            reinterpret_cast<uint64_t>(&current_command_buffer_),
        .swapchain_image_index = image_index,
    };

    CardboardDistortionRenderer_renderEyeToDisplay(
        renderer, reinterpret_cast<uint64_t>(&target_config),
        screen_params.viewport_x, screen_params.viewport_y,
        screen_params.viewport_width, screen_params.viewport_height, left_eye,
        right_eye);
  }

  void RunRenderingPreProcessing(const ScreenParams& screen_params) override {
    UnityVulkanRecordingState vulkanRecordingState;
    vulkan_interface_->EnsureOutsideRenderPass();
    vulkan_interface_->CommandRecordingState(
        &vulkanRecordingState, kUnityVulkanGraphicsQueueAccess_DontCare);

    current_command_buffer_ = vulkanRecordingState.commandBuffer;
    // If width or height of the rendering area changes, then we need to
    // recreate all frame buffers.
    if (screen_params.viewport_width != current_rendering_width_ ||
        screen_params.viewport_height != current_rendering_height_) {
      frames_to_update_count_ = swapchain_image_count_;
      current_rendering_width_ = screen_params.viewport_width;
      current_rendering_height_ = screen_params.viewport_height;
    }

    if (frames_to_update_count_ > 0) {
      if (frame_buffers_[image_index] != VK_NULL_HANDLE) {
        cardboard::rendering::vkDestroyFramebuffer(
            logical_device_, frame_buffers_[image_index], nullptr);
      }

      VkImageView attachments[] = {swapchain_views_[image_index]};
      VkFramebufferCreateInfo fb_create_info{
          .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
          .pNext = nullptr,
          .renderPass = render_pass_,
          .attachmentCount = 1,
          .pAttachments = attachments,
          .width = static_cast<uint32_t>(screen_params.width),
          .height = static_cast<uint32_t>(screen_params.height),
          .layers = 1,
      };

      cardboard::rendering::vkCreateFramebuffer(
          logical_device_, &fb_create_info, nullptr /* pAllocator */,
          &frame_buffers_[image_index]);
      frames_to_update_count_--;
    }

    // Begin RenderPass
    const VkClearValue clear_vals = {
        .color = {.float32 = {0.0f, 0.0f, 0.0f, 1.0f}}};
    const VkRenderPassBeginInfo render_pass_begin_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .pNext = nullptr,
        .renderPass = render_pass_,
        .framebuffer = frame_buffers_[image_index],
        .renderArea = {.offset =
                           {
                               .x = 0,
                               .y = 0,
                           },
                       .extent =
                           {
                               .width =
                                   static_cast<uint32_t>(screen_params.width),
                               .height =
                                   static_cast<uint32_t>(screen_params.height),
                           }},
        .clearValueCount = 1,
        .pClearValues = &clear_vals};
    cardboard::rendering::vkCmdBeginRenderPass(current_command_buffer_,
                                               &render_pass_begin_info,
                                               VK_SUBPASS_CONTENTS_INLINE);
  }

  void RunRenderingPostProcessing() override {
    cardboard::rendering::vkCmdEndRenderPass(current_command_buffer_);
  }

 private:
  /**
   * Find the memory type of the physical device.
   *
   * @param type_filter required memory type shift.
   * @param properties required memory flag bits.
   *
   * @return memory type or 0 if not found.
   */
  uint32_t FindMemoryType(uint32_t type_filter,
                          VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    cardboard::rendering::vkGetPhysicalDeviceMemoryProperties(physical_device_,
                                                              &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
      if ((type_filter & (1 << i)) &&
          (memProperties.memoryTypes[i].propertyFlags & properties) ==
              properties) {
        return i;
      }
    }

    CARDBOARD_LOGE("failed to find suitable memory type!");
    return 0;
  }

  // Variables created externally.
  int current_rendering_width_;
  int current_rendering_height_;
  IUnityGraphicsVulkanV2* vulkan_interface_{nullptr};
  VkPhysicalDevice physical_device_;
  VkDevice logical_device_;
  VkCommandBuffer current_command_buffer_;
  std::vector<VkImage> swapchain_images_;

  // Variables created and maintained by the vulkan renderer.
  uint32_t swapchain_image_count_;
  uint32_t frames_to_update_count_;
  VkRenderPass render_pass_;
  std::vector<VkImageView> swapchain_views_;
  std::vector<VkFramebuffer> frame_buffers_;
  std::unique_ptr<VulkanWidgetsRenderer> widget_renderer_;
};

}  // namespace

std::unique_ptr<Renderer> MakeVulkanRenderer(IUnityInterfaces* xr_interfaces) {
  return std::make_unique<VulkanRenderer>(xr_interfaces);
}

CardboardDistortionRenderer* MakeCardboardVulkanDistortionRenderer(
    IUnityInterfaces* xr_interfaces) {
  IUnityGraphicsVulkanV2* vulkan_interface =
      xr_interfaces->Get<IUnityGraphicsVulkanV2>();
  UnityVulkanInstance vulkan_instance = vulkan_interface->Instance();
  const CardboardVulkanDistortionRendererConfig config{
      .physical_device =
          reinterpret_cast<uint64_t>(&vulkan_instance.physicalDevice),
      .logical_device = reinterpret_cast<uint64_t>(&vulkan_instance.device),
      .vk_swapchain = reinterpret_cast<uint64_t>(&cached_swapchain),
  };

  CardboardDistortionRenderer* distortion_renderer =
      CardboardVulkanDistortionRenderer_create(&config);
  return distortion_renderer;
}

}  // namespace cardboard::unity
