#include <cglm/cam.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_core.h>
#define GLFW_INCLUDE_VULKAN
#include "./model.h"
#include <GLFW/glfw3.h>

// We'll make constant sized arrays and put them on the stack when we can
// so we dont have to deal with heap allocation. This controls the
// max number of swap chain images we want to support by controlling the
// length of those arrays
#define MAX_SWAPCHAIN_IMAGES 8

typedef struct CameraState {
  // Loaded onto GPU
  mat4 model;
  mat4 view;
  mat4 proj;
  // Doesn't go to GPU
  vec3 cameraVelocity;
  bool cameraTurning; // True = Holding down camera turn modifier
} CameraState;

typedef struct InputState {
  vec4 mouse;
  vec2 windowSize;
  uint32_t mouseButtons;
  uint32_t selectionBufferLength;
	uint32_t frameId; // Rolling frame timestamp for determining how recently a selection occurred
  char selectionMap[4096];
  char selectionBuffer[256];
} InputState;

typedef struct GraphicsState {
  VkCommandBuffer commandbuffers[MAX_SWAPCHAIN_IMAGES];
  VkCommandPool commandPool;
  VkInstance instance;
  VkPhysicalDevice physicalDevice;
  VkDevice device;
  GLFWwindow *window;
  VkSurfaceKHR surface;
  VkFramebuffer framebuffers[MAX_SWAPCHAIN_IMAGES];
  VkSwapchainKHR swapchain;
  VkExtent2D renderArea;
  VkRenderPass renderPass;
  uint32_t imageCount;
  // Indexed by imageId (incrementing index) divorced from actual
  // image id
  VkSemaphore imageReadySemaphores[MAX_SWAPCHAIN_IMAGES];
  VkSemaphore renderFinishedSemaphores[MAX_SWAPCHAIN_IMAGES];
  VkFence imageReadyFences[MAX_SWAPCHAIN_IMAGES];
  // Incrementing counter for next to render image
  uint32_t imageId;
  VkImage swapchainImages[MAX_SWAPCHAIN_IMAGES];
  VkImage depthImages[MAX_SWAPCHAIN_IMAGES];
  VkDeviceMemory depthImageMemories[MAX_SWAPCHAIN_IMAGES];
  // Pipeline
  VkPipeline graphicsPipelines[2];
  VkPipelineLayout layout;
  VkDescriptorPool descriptorPool;
  VkDescriptorSet descriptorSets[MAX_SWAPCHAIN_IMAGES];
  VkDescriptorSetLayout descriptorSetLayout;
  VkBuffer cameraBuffer;
  VkDeviceMemory cameraMemory;
  VkBuffer inputBuffer;
  VkDeviceMemory inputMemory;
  VkFence inputReadFence;
  VkBuffer stagingInputBuffer;
  VkDeviceMemory stagingInputMemory;
  // Model loading stuff
  VkDeviceMemory stagingMemory;
  VkBuffer stagingBuffer;
  VkFence stagingFence;
  // Entities
  EntityDef *entities;
  size_t entityCount;
  size_t maxEntities;
  VkFence entitySyncFence;
  VkCommandBuffer entitySyncCommandBuffer;
  VkCommandBuffer inputReadCommandBuffer;
  bool commandBufferDirty;
  bool entitiesSyncing;
  CameraState *camera;
  InputState *input;
} GraphicsState;

uint32_t *getMemoryTypeMatching(VkPhysicalDevice physicalDevice,
                                VkMemoryPropertyFlags flags,
                                uint32_t *memoryTypeCount) {
  VkPhysicalDeviceMemoryProperties memoryProperties;
  static uint32_t memoryTypes[128];
  vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
  uint32_t i = 0;
  uint32_t count = 0;
  for (i = 0; i < memoryProperties.memoryTypeCount; i++) {
    if ((flags & memoryProperties.memoryTypes[i].propertyFlags) == flags) {
      memoryTypes[count++] = i;
    }
  }
  if (memoryTypeCount) {
    *memoryTypeCount = count;
  }
  return memoryTypes;
}

uint32_t *getQueuesMatching(VkPhysicalDevice physicalDevice, VkQueueFlags flags,
                            uint32_t *queueCount) {
  static uint32_t queueIds[128];
  static uint32_t count = 0;
  static VkQueueFamilyProperties properties[128];
  vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &count, properties);
  for (uint32_t i = 0; i < count; i++) {
    if ((properties[i].queueFlags & flags) == flags) {
      queueIds[count] = i;
      count++;
    }
  }
  if (queueCount != 0) {
    *queueCount = count;
  }
  return queueIds;
}

void SetupCommandBuffer(
    GraphicsState *state, int frameNumber,
    /*
     * VkCommandBuffer commandbuffer, VkRenderPass renderpass,
            VkPipeline graphicsPipelines[2],
            VkFramebuffer framebuffer, VkPipelineLayout layout,
            VkDescriptorSet descriptorSet, VkExtent2D extent,*/
    EntityDef *entities, uint32_t entityCount) {
  vkBeginCommandBuffer(state->commandbuffers[frameNumber],
                       &(VkCommandBufferBeginInfo){
                           .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                       });
  vkCmdBindPipeline(state->commandbuffers[frameNumber],
                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                    state->graphicsPipelines[0]);
  vkCmdBeginRenderPass(
      state->commandbuffers[frameNumber],
      &(VkRenderPassBeginInfo){
          .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
          .renderPass = state->renderPass,
          .framebuffer = state->framebuffers[frameNumber],
          .renderArea = {.offset = {0., 0.}, .extent = state->renderArea},
          .clearValueCount = 2,
          .pClearValues =
              (VkClearValue[2]){{.color = {.float32 = {0., 0., 0., 255.}}},
                                {.depthStencil =
                                     {
                                         .depth = 1.,
                                     }}}},
      VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindDescriptorSets(state->commandbuffers[frameNumber],
                          VK_PIPELINE_BIND_POINT_GRAPHICS, state->layout, 0, 1,
                          &state->descriptorSets[frameNumber], 0,
                          VK_NULL_HANDLE);
  for (uint32_t i = 0; i < entityCount; i++) {
    if (entities[i].instanceCount == 0) {
      continue;
    }
    vkCmdBindVertexBuffers(state->commandbuffers[frameNumber], 0, 2,
                           (VkBuffer[2]){entities[i].model.vertexBuffer,
                                         entities[i].instanceBuffer},
                           (VkDeviceSize[2]){0, 0});
    vkCmdDraw(state->commandbuffers[frameNumber], entities[i].model.vertexCount,
              entities[i].instanceCount, 0, 0);
  }
  vkCmdEndRenderPass(state->commandbuffers[frameNumber]);
  vkEndCommandBuffer(state->commandbuffers[frameNumber]);
}

VkShaderModule LoadShaderFromFile(VkDevice device, char *filepath) {
  VkShaderModule module;
  uint32_t *code;
  if (!filepath) {
    fprintf(stderr, "Attempted to read shader from NULL filepath\n");
  }
  FILE *file = fopen(filepath, "r");
  fseek(file, 0, SEEK_END);
  uint32_t length = ftell(file);
  code = malloc(sizeof(char) * length);
  fseek(file, 0, SEEK_SET);
  fread(code, sizeof(char), length, file);
  vkCreateShaderModule(device,
                       &(VkShaderModuleCreateInfo){
                           .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                           .codeSize = length,
                           .pCode = code},
                       NULL, &module);
  free(code);
  return module;
}

void CreateBuffer(VkDevice device, VkPhysicalDevice physical, size_t size,
                  VkMemoryPropertyFlagBits memoryFlags,
                  VkBufferUsageFlagBits usageFlags, VkBuffer *buffer,
                  VkDeviceMemory *memory) {
  VkPhysicalDeviceMemoryProperties properties;
  vkGetPhysicalDeviceMemoryProperties(physical, &properties);
  uint32_t memoryIndex = 0;
  for (memoryIndex = 0; memoryIndex < properties.memoryTypeCount;
       memoryIndex++) {
    if ((properties.memoryTypes[memoryIndex].propertyFlags & memoryFlags) ==
        memoryFlags) {
      break;
    }
  }
  vkAllocateMemory(
      device,
      &(VkMemoryAllocateInfo){.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                              .allocationSize = size,
                              .memoryTypeIndex = memoryIndex},
      NULL, memory);
  vkCreateBuffer(device,
                 &(VkBufferCreateInfo){
                     .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                     .size = size,
                     .usage = usageFlags,
                     .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                     .queueFamilyIndexCount = 1,
                     .pQueueFamilyIndices =
                         getQueuesMatching(physical, VK_QUEUE_TRANSFER_BIT, 0)},
                 NULL, buffer);
  vkBindBufferMemory(device, *buffer, *memory, 0);
}

void UpdateInputState(GraphicsState *state) {

  state->input->windowSize[0] = state->renderArea.width;
  state->input->windowSize[1] = state->renderArea.height;
	if(state->input->frameId == 15) {
		state->input->frameId = 1;
	} else {
		state->input->frameId++;
	}

  // Buttons
  uint32_t mouseButtons =
      (glfwGetMouseButton(state->window, GLFW_MOUSE_BUTTON_LEFT) << 1) |
      glfwGetMouseButton(state->window, GLFW_MOUSE_BUTTON_RIGHT);
  if (mouseButtons != state->input->mouseButtons) {
    state->input->mouseButtons = mouseButtons;
  }

  // Cursor
  double xpos, ypos;
  glfwGetCursorPos(state->window, &xpos, &ypos);
  if (state->input->mouse[0] != xpos || state->input->mouse[1] != ypos) {
  }
  state->input->mouse[0] = xpos;
  state->input->mouse[1] = ypos;

  // Box drag start
  if ((state->input->mouseButtons & 2) == 0) {
    state->input->mouse[2] = state->input->mouse[0];
    state->input->mouse[3] = state->input->mouse[1];
  } 
}

void UploadModel(GraphicsState *state, Model *model) {
  VkCommandBuffer commandBuffer;
  vkAllocateCommandBuffers(
      state->device,
      &(VkCommandBufferAllocateInfo){
          .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
          .commandPool = state->commandPool,
          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
          .commandBufferCount = 1},
      &commandBuffer);
  VkQueue queue;
  // TODO: We're probably doubling up our transfers on the graphics queue
  vkGetDeviceQueue(
      state->device,
      getQueuesMatching(state->physicalDevice, VK_QUEUE_TRANSFER_BIT, 0)[0], 0,
      &queue);
  CreateBuffer(
      state->device, state->physicalDevice, model->vertexCount * sizeof(Vertex),
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
      VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
      &model->vertexBuffer, &model->vertexMemory);

  vkWaitForFences(state->device, 1, &state->stagingFence, 1, 0);
  {
    vkResetFences(state->device, 1, &state->stagingFence);
    void *pp;
    vkMapMemory(state->device, state->stagingMemory, 0, VK_WHOLE_SIZE, 0, &pp);
    memcpy(pp, model->vertices, sizeof(Vertex) * model->vertexCount);
    vkUnmapMemory(state->device, state->stagingMemory);
    vkBeginCommandBuffer(
        commandBuffer,
        &(VkCommandBufferBeginInfo){
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO});
    vkCmdCopyBuffer(
        commandBuffer, state->stagingBuffer, model->vertexBuffer, 1,
        &(VkBufferCopy){.srcOffset = 0,
                        .dstOffset = 0,
                        .size = sizeof(Vertex) * model->vertexCount});
    vkEndCommandBuffer(commandBuffer);
    vkQueueSubmit(queue, 1,
                  &(VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                  .waitSemaphoreCount = 0,
                                  .commandBufferCount = 1,
                                  .pCommandBuffers = &commandBuffer,
                                  .signalSemaphoreCount = 0},
                  state->stagingFence);
  }
}

uint32_t CreateEntityDef(GraphicsState *state, Model *model) {
  if (state->maxEntities < state->entityCount + 1) {
    state->maxEntities = state->maxEntities + 512;
    // TODO: Hm this looks very thread safe
    state->entities =
        realloc(state->entities, sizeof(EntityDef) * state->maxEntities);
  }
  state->entities[state->entityCount] = (EntityDef){
      .model = *model,
      .instances = calloc(64, sizeof(Instance)),
      .dirtyBuffer = calloc(64, sizeof(bool)),
      .maxInstances = 64,
  };
  UploadModel(state, &state->entities[state->entityCount].model);
  return state->entityCount++;
}

uint32_t AddEntityInstance(EntityDef *entity, Instance instance) {
  static uint32_t instanceId = 0;
  instance.instanceId = instanceId++;

  if (entity->maxInstances > entity->instanceCount) {
    entity->maxInstances = entity->maxInstances + 64;
    entity->instances =
        realloc(entity->instances, sizeof(Instance) * entity->maxInstances);
    entity->dirtyBuffer =
        realloc(entity->dirtyBuffer, sizeof(Instance) * entity->maxInstances);
  }
  entity->instances[entity->instanceCount] = instance;
  entity->dirtyBuffer[entity->instanceCount] = true;
  return entity->instanceCount++;
}

// Allocate and update all the instance data for all entities
// Also update uniform buffer for camera data
// Returns 1 if still syncing, 0 if success
// TODO: This is kinda shitty and looks nothing like I envisioned it to
uint32_t UpdateGraphicsMemory(GraphicsState *state) {
  // Wait for (potential) last update to finis
  if (!state->entitySyncFence) {
    vkCreateFence(
        state->device,
        &(VkFenceCreateInfo){.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO},
        NULL, &state->entitySyncFence);
  }
  if (state->entitiesSyncing) {
    VkResult res =
        vkWaitForFences(state->device, 1, &state->entitySyncFence, VK_TRUE, 0);
    if (res == VK_TIMEOUT) {
      return 1;
    }
    vkResetFences(state->device, 1, &state->entitySyncFence);
    state->entitiesSyncing = false;
  }
  // Reset command buffer/create new command buffer
  if (!state->entitySyncCommandBuffer) {
    vkAllocateCommandBuffers(
        state->device,
        &(VkCommandBufferAllocateInfo){
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = state->commandPool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1},
        &state->entitySyncCommandBuffer);
  } else {
    vkResetCommandBuffer(state->entitySyncCommandBuffer, 0);
  }
  vkBeginCommandBuffer(
      state->entitySyncCommandBuffer,
      &(VkCommandBufferBeginInfo){
          .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO});
  // Iterate over entity defs
  for (uint32_t t = 0; t < state->entityCount; t++) {
    EntityDef *def = &state->entities[t];
    // Setup entity def's instance buffer if necessary
    if (!def->instanceBuffer) {
      CreateBuffer(state->device, state->physicalDevice, sizeof(Instance) * 64,
                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                   &def->instanceBuffer, &def->instanceMemory);
    }
    int32_t start = -1;
    for (uint32_t i = 0; i < def->instanceCount; i++) {
      if (def->dirtyBuffer[i] && start == -1) {
        start = i;
      }
      if ((i == (def->instanceCount - 1) || !def->dirtyBuffer[i]) &&
          start != -1) {
        vkCmdUpdateBuffer(state->entitySyncCommandBuffer, def->instanceBuffer,
                          sizeof(Instance) * start,
                          (i + 1 - start) * sizeof(Instance),
                          &def->instances[start]);
        start = -1;
      }
      def->dirtyBuffer[i] = false;
    }
  }
  vkEndCommandBuffer(state->entitySyncCommandBuffer);
  VkQueue queue;
  // TODO: We're probably doubling up our transfers on the graphics queue
  vkGetDeviceQueue(
      state->device,
      getQueuesMatching(state->physicalDevice, VK_QUEUE_TRANSFER_BIT, 0)[0], 0,
      &queue);
  // Submit updates
  vkQueueSubmit(
      queue, 1,
      &(VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                      .commandBufferCount = 1,
                      .pCommandBuffers = &state->entitySyncCommandBuffer},
      state->entitySyncFence);
  state->entitiesSyncing = true;
  return 0;
}

void CreateRenderState(GraphicsState *state) {

  VkSurfaceCapabilitiesKHR capabilites;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(state->physicalDevice,
                                            state->surface, &capabilites);
  state->renderArea = capabilites.maxImageExtent;
  uint32_t count;
  vkGetPhysicalDeviceSurfaceFormatsKHR(state->physicalDevice, state->surface,
                                       &count, 0);
  VkSurfaceFormatKHR *formats = calloc(count, sizeof(VkSurfaceFormatKHR));
  vkGetPhysicalDeviceSurfaceFormatsKHR(state->physicalDevice, state->surface,
                                       &count, formats);
  vkGetPhysicalDeviceSurfacePresentModesKHR(state->physicalDevice,
                                            state->surface, &count, 0);
  VkBool32 supported;
  vkGetPhysicalDeviceSurfaceSupportKHR(
      state->physicalDevice,
      *getQueuesMatching(state->physicalDevice, VK_QUEUE_GRAPHICS_BIT, 0),
      state->surface, &supported);
  if (!supported) {
    fprintf(stderr, "Surface does not support swapchain! I dont know why :(\n");
    exit(1);
  }
  vkCreateSwapchainKHR(
      state->device,
      &(VkSwapchainCreateInfoKHR){
          .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
          .surface = state->surface,
          .minImageCount = capabilites.minImageCount,
          .imageFormat = formats[0].format,
          .imageColorSpace = formats[0].colorSpace,
          .imageExtent = capabilites.currentExtent,
          .imageArrayLayers = 1,
          .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
          .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
          .queueFamilyIndexCount = 1,
          .pQueueFamilyIndices = getQueuesMatching(state->physicalDevice,
                                                   VK_QUEUE_GRAPHICS_BIT, 0),
          .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
          .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
          .presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR,
          .clipped = VK_TRUE},
      0, &state->swapchain);

  vkCreateRenderPass(
      state->device,
      &(VkRenderPassCreateInfo){
          .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
          .attachmentCount = 2,
          .pAttachments =
              (VkAttachmentDescription[2]){
                  {.format = formats[0].format,
                   .samples = VK_SAMPLE_COUNT_1_BIT,
                   .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                   .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                   .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                   .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR},
                  {.format = VK_FORMAT_D32_SFLOAT,
                   .samples = VK_SAMPLE_COUNT_1_BIT,
                   .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                   .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                   .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                   .finalLayout = VK_IMAGE_LAYOUT_GENERAL}},
          .subpassCount = 1,
          .pSubpasses =
              (VkSubpassDescription[1]){
                  {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                   .colorAttachmentCount = 1,
                   .pColorAttachments =
                       &(VkAttachmentReference){
                           0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                   .pDepthStencilAttachment =
                       &(VkAttachmentReference){
                           1,
                           VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}}},
          .dependencyCount = 1,
          .pDependencies =
              (VkSubpassDependency[1]){
                  {.srcSubpass = VK_SUBPASS_EXTERNAL,
                   .dstSubpass = 0,
                   .srcStageMask =
                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                   .srcAccessMask = 0,
                   .dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                   .dstAccessMask =
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT}}},
      0, &state->renderPass);

  vkGetSwapchainImagesKHR(state->device, state->swapchain, &count, NULL);
  vkGetSwapchainImagesKHR(state->device, state->swapchain, &count,
                          state->swapchainImages);
  state->imageCount = count;
  vkAllocateCommandBuffers(
      state->device,
      &(VkCommandBufferAllocateInfo){
          .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
          .commandPool = state->commandPool,
          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
          .commandBufferCount = count},
      state->commandbuffers);

  // Descriptor Set
  vkCreateDescriptorSetLayout(
      state->device,
      &(VkDescriptorSetLayoutCreateInfo){
          .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
          .bindingCount = 2,
          .pBindings =
              (VkDescriptorSetLayoutBinding[2]){
                  {.binding = 0,
                   .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                   .descriptorCount = 1,
                   .stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
                                 VK_SHADER_STAGE_FRAGMENT_BIT},
                  {.binding = 1,
                   .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                   .descriptorCount = 1,
                   .stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
                                 VK_SHADER_STAGE_FRAGMENT_BIT}}},
      NULL, &state->descriptorSetLayout);
  vkCreateDescriptorPool(
      state->device,
      &(VkDescriptorPoolCreateInfo){
          .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
          .maxSets = MAX_SWAPCHAIN_IMAGES,
          .poolSizeCount = 2,
          .pPoolSizes =
              (VkDescriptorPoolSize[2]){
                  {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                   .descriptorCount = MAX_SWAPCHAIN_IMAGES},
                  {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                   .descriptorCount = MAX_SWAPCHAIN_IMAGES}}},
      NULL, &state->descriptorPool);
  VkWriteDescriptorSet vwds[MAX_SWAPCHAIN_IMAGES * 2];
  vkAllocateDescriptorSets(
      state->device,
      &(VkDescriptorSetAllocateInfo){
          .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
          .descriptorPool = state->descriptorPool,
          .descriptorSetCount = state->imageCount,
          .pSetLayouts =
              (VkDescriptorSetLayout[MAX_SWAPCHAIN_IMAGES]){
                  state->descriptorSetLayout, state->descriptorSetLayout,
                  state->descriptorSetLayout}},
      state->descriptorSets);
  // Per image state
  for (uint32_t i = 0; i < state->imageCount; i++) {
    vkCreateImage(state->device,
                  &(VkImageCreateInfo){
                      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                      .imageType = VK_IMAGE_TYPE_2D,
                      .format = VK_FORMAT_D32_SFLOAT,
                      .extent = {.width = capabilites.maxImageExtent.width,
                                 .height = capabilites.maxImageExtent.height,
                                 .depth = 1},
                      .mipLevels = 1,
                      .arrayLayers = 1,
                      .samples = 1,
                      .tiling = VK_IMAGE_TILING_OPTIMAL,
                      .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                      .sharingMode = VK_SHARING_MODE_EXCLUSIVE},
                  NULL, &state->depthImages[i]);
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(state->device, state->depthImages[i],
                                 &requirements);
    vkAllocateMemory(
        state->device,
        &(VkMemoryAllocateInfo){
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size,
            .memoryTypeIndex = getMemoryTypeMatching(
                state->physicalDevice, requirements.memoryTypeBits, 0)[0]},
        NULL, &state->depthImageMemories[i]);
    vkBindImageMemory(state->device, state->depthImages[i],
                      state->depthImageMemories[i], 0);
    VkImageView imageView[2];
    vkCreateImageView(
        state->device,
        &(VkImageViewCreateInfo){
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = state->swapchainImages[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = formats[0].format,
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .baseMipLevel = 0,
                                 .levelCount = 1,
                                 .baseArrayLayer = 0,
                                 .layerCount = 1}},
        0, &imageView[0]);
    vkCreateImageView(
        state->device,
        &(VkImageViewCreateInfo){
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = state->depthImages[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_D32_SFLOAT,
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                                 .baseMipLevel = 0,
                                 .levelCount = 1,
                                 .baseArrayLayer = 0,
                                 .layerCount = 1}},
        0, &imageView[1]);
    vwds[i * 2] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = state->descriptorSets[i],
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .dstBinding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .pBufferInfo =
            (VkDescriptorBufferInfo[1]){{.buffer = state->cameraBuffer,
                                         .offset = 0,
                                         .range = sizeof(CameraState)}}};
    vwds[i * 2 + 1] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = state->descriptorSets[i],
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .dstBinding = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo =
            (VkDescriptorBufferInfo[1]){{.buffer = state->inputBuffer,
                                         .offset = 0,
                                         .range = sizeof(InputState)}}};
    vkCreateFramebuffer(state->device,
                        &(VkFramebufferCreateInfo){
                            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                            .renderPass = state->renderPass,
                            .attachmentCount = 2,
                            .pAttachments = imageView,
                            .width = capabilites.maxImageExtent.width,
                            .height = capabilites.maxImageExtent.height,
                            .layers = 1},
                        0, &state->framebuffers[i]);
  }
  vkUpdateDescriptorSets(state->device, state->imageCount * 2, vwds, 0,
                         VK_NULL_HANDLE);

  vkCreatePipelineLayout(
      state->device,
      &(VkPipelineLayoutCreateInfo){
          .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
          .setLayoutCount = 1,
          .pSetLayouts = (VkDescriptorSetLayout[1]){state->descriptorSetLayout},
          .pushConstantRangeCount = 0,
          .pPushConstantRanges = NULL},
      NULL, &state->layout);
  vkCreateGraphicsPipelines(
      state->device, VK_NULL_HANDLE, 1,
      (VkGraphicsPipelineCreateInfo[1]){
          {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
           .stageCount = 2,
           .pStages =
               (VkPipelineShaderStageCreateInfo[2]){
                   {.sType =
                        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_VERTEX_BIT,
                    .module = LoadShaderFromFile(state->device,
                                                 "./shaders/vertex.spv"),
                    .pName = "main"},
                   {.sType =
                        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                    .module = LoadShaderFromFile(state->device,
                                                 "./shaders/fragment.spv"),
                    .pName = "main"}},
           .pVertexInputState =
               &(VkPipelineVertexInputStateCreateInfo){
                   .sType =
                       VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                   .vertexBindingDescriptionCount = 2,
                   .pVertexBindingDescriptions =
                       (VkVertexInputBindingDescription[2]){
                           {.binding = 0,
                            .stride = sizeof(Vertex),
                            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX},
                           {.binding = 1,
                            .stride = sizeof(Instance),
                            .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE}},
                   .vertexAttributeDescriptionCount = 11,
                   .pVertexAttributeDescriptions =
                       (VkVertexInputAttributeDescription[11]){
                           {.location = 0,
                            .binding = 0,
                            .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                            .offset = offsetof(Vertex, position)},
                           {.location = 1,
                            .binding = 0,
                            .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                            .offset = offsetof(Vertex, color)},
                           {.location = 2,
                            .binding = 0,
                            .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                            .offset = offsetof(Vertex, normal)},
                           {.location = 3,
                            .binding = 1,
                            .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                            .offset = offsetof(Instance, rotation[0])},
                           {.location = 4,
                            .binding = 1,
                            .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                            .offset = offsetof(Instance, rotation[1])},
                           {.location = 5,
                            .binding = 1,
                            .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                            .offset = offsetof(Instance, rotation[2])},
                           {.location = 6,
                            .binding = 1,
                            .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                            .offset = offsetof(Instance, rotation[3])},
                           {.location = 7,
                            .binding = 1,
                            .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                            .offset = offsetof(Instance, position)},
                           {.location = 8,
                            .binding = 1,
                            .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                            .offset = offsetof(Instance, scale)},
                           {.location = 9,
                            .binding = 1,
                            .format = VK_FORMAT_R32_UINT,
                            .offset = offsetof(Instance, instanceId)},
                           {.location = 10,
                            .binding = 1,
                            .format = VK_FORMAT_R32_UINT,
                            .offset = offsetof(Instance, selected)}}},
           .pInputAssemblyState =
               &(VkPipelineInputAssemblyStateCreateInfo){
                   .sType =
                       VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                   .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST},
           .pViewportState =
               &(VkPipelineViewportStateCreateInfo){
                   .sType =
                       VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                   .viewportCount = 1,
                   .pViewports = &(
                       VkViewport){.x = 0,
                                   .y = 0,
                                   .width = capabilites.maxImageExtent.width,
                                   .height = capabilites.maxImageExtent.height,
                                   .minDepth = 0,
                                   .maxDepth = 1},
                   .scissorCount = 1,
                   .pScissors =
                       &(VkRect2D){.extent = capabilites.maxImageExtent,
                                   .offset = {0, 0}}},
           .pRasterizationState =
               &(VkPipelineRasterizationStateCreateInfo){
                   .sType =
                       VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                   .depthClampEnable = VK_FALSE,
                   .rasterizerDiscardEnable = VK_FALSE,
                   .polygonMode = VK_POLYGON_MODE_FILL,
                   .lineWidth = 1.f,
                   .cullMode = VK_CULL_MODE_BACK_BIT,
                   .frontFace = VK_FRONT_FACE_CLOCKWISE},
           .pMultisampleState =
               &(VkPipelineMultisampleStateCreateInfo){
                   .sType =
                       VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                   .sampleShadingEnable = VK_FALSE,
                   .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
                   .minSampleShading = 1.f},
           .pDepthStencilState =
               &(VkPipelineDepthStencilStateCreateInfo){
                   .sType =
                       VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                   .depthTestEnable = VK_TRUE,
                   .depthWriteEnable = VK_TRUE,
                   .depthCompareOp = VK_COMPARE_OP_LESS,
                   .depthBoundsTestEnable = VK_FALSE,
                   .stencilTestEnable = VK_FALSE,
                   .front = {.failOp = VK_STENCIL_OP_KEEP,
                             .passOp = VK_STENCIL_OP_KEEP,
                             .compareOp = VK_COMPARE_OP_ALWAYS},
                   .back = {.failOp = VK_STENCIL_OP_KEEP,
                            .passOp = VK_STENCIL_OP_KEEP,
                            .compareOp = VK_COMPARE_OP_ALWAYS},
                   .minDepthBounds = -5,
                   .maxDepthBounds = 100},
           .pColorBlendState =
               &(VkPipelineColorBlendStateCreateInfo){
                   .sType =
                       VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                   .logicOpEnable = VK_FALSE,
                   .attachmentCount = 1,
                   .pAttachments =
                       &(VkPipelineColorBlendAttachmentState){
                           .colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                             VK_COLOR_COMPONENT_G_BIT |
                                             VK_COLOR_COMPONENT_B_BIT |
                                             VK_COLOR_COMPONENT_A_BIT,
                           .blendEnable = VK_TRUE,
                           .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
                           .dstColorBlendFactor =
                               VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                           .colorBlendOp = VK_BLEND_OP_ADD,
                           .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                           .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
                           .alphaBlendOp = VK_BLEND_OP_ADD},
                   .blendConstants = {0.f, 0.f, 0.f, 0.f}},
           .pDynamicState = NULL,
           .layout = state->layout,
           .renderPass = state->renderPass,
           .subpass = 0},
      },
      NULL, state->graphicsPipelines);
  glm_perspective(80,
                  (float)capabilites.currentExtent.width /
                      (float)capabilites.currentExtent.height,
                  0.1, 50, state->camera->proj);
}

void CleanRenderState(GraphicsState *state) {
  vkFreeCommandBuffers(state->device, state->commandPool, state->imageCount,
                       state->commandbuffers);
  for (uint32_t i = 0; i < state->imageCount; i++) {
    vkDestroyFramebuffer(state->device, state->framebuffers[i], NULL);
  }
  vkDestroyDescriptorPool(state->device, state->descriptorPool, NULL);
  vkDestroySwapchainKHR(state->device, state->swapchain, NULL);
  vkDestroyRenderPass(state->device, state->renderPass, NULL);
  vkDestroyPipeline(state->device, state->graphicsPipelines[0], NULL);
  vkDestroyPipeline(state->device, state->graphicsPipelines[1], NULL);
  state->commandBufferDirty = true;
}

GraphicsState InitGraphics() {
  uint32_t count;
  glfwInit();
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  VkInstance instance;
  {
    const char **extensions = glfwGetRequiredInstanceExtensions(&count);
    vkCreateInstance(
        &(VkInstanceCreateInfo){.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                .enabledExtensionCount = count,
                                .ppEnabledExtensionNames = extensions},
        0, &instance);
  }
  GLFWwindow *window =
      glfwCreateWindow(600, 400, "Real Life Fantasy Battles", 0, 0);

  VkPhysicalDevice physicalDevice = NULL;
  {
    vkEnumeratePhysicalDevices(instance, &count, 0);
    VkPhysicalDevice *physicalDevices = calloc(count, sizeof(VkPhysicalDevice));
    vkEnumeratePhysicalDevices(instance, &count, physicalDevices);
    for (uint32_t i = 0; i < count; i++) {
      physicalDevice = physicalDevices[i];
      VkPhysicalDeviceFeatures features;
      vkGetPhysicalDeviceFeatures(physicalDevice, &features);
      if (!features.fragmentStoresAndAtomics) {
        printf("Rejecting device, lacks fragmentStoresAndAtomics\n");
        physicalDevice = NULL;
        continue;
      } else {
        break;
      }
    }
  }
  if (physicalDevice == NULL) {
    fprintf(stderr, "Unable to find good enough physical device\n");
  }

  uint32_t *queues =
      getQueuesMatching(physicalDevice, VK_QUEUE_GRAPHICS_BIT, 0);
  VkDevice device;
  vkCreateDevice(
      physicalDevice,
      &(VkDeviceCreateInfo){
          .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
          .pEnabledFeatures =
              &(VkPhysicalDeviceFeatures){.fragmentStoresAndAtomics = true},
          .enabledExtensionCount = 1,
          .ppEnabledExtensionNames =
              &(const char *){VK_KHR_SWAPCHAIN_EXTENSION_NAME},
          .queueCreateInfoCount = 1,
          .pQueueCreateInfos =
              &(VkDeviceQueueCreateInfo){
                  .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                  .queueFamilyIndex = queues[0],
                  .queueCount = 1,
                  .pQueuePriorities = &(float){1.}}

      },
      0, &device);
  VkSurfaceKHR surface;
  VkResult res;
  res = glfwCreateWindowSurface(instance, window, 0, &surface);
  if (res != VK_SUCCESS) {
    fprintf(stderr, "Failed to create surface: %d\n", res);
    exit(1);
  }

  GraphicsState state =
      (GraphicsState){.instance = instance,
                      .window = window,
                      .surface = surface,
                      .physicalDevice = physicalDevice,
                      .device = device,
                      .entities = calloc(128, sizeof(EntityDef)),
                      .maxEntities = 128,
                      .commandBufferDirty = true};

  for (uint32_t i = 0; i < MAX_SWAPCHAIN_IMAGES; i++) {
    vkCreateSemaphore(device,
                      &(VkSemaphoreCreateInfo){
                          .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO},
                      NULL, &state.imageReadySemaphores[i]);
    vkCreateSemaphore(device,
                      &(VkSemaphoreCreateInfo){
                          .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO},
                      NULL, &state.renderFinishedSemaphores[i]);
    vkCreateFence(
        state.device,
        &(VkFenceCreateInfo){.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO},
        NULL, &state.imageReadyFences[i]);
  }
  vkCreateCommandPool(
      state.device,
      &(VkCommandPoolCreateInfo){
          .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
          .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
          .queueFamilyIndex = getQueuesMatching(state.physicalDevice,
                                                VK_QUEUE_GRAPHICS_BIT, 0)[0]},
      0, &state.commandPool);

  // Model loading
  CreateBuffer(device, physicalDevice, 500000 * sizeof(Vertex),
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
               VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &state.stagingBuffer,
               &state.stagingMemory);
  vkCreateFence(
      state.device,
      &(VkFenceCreateInfo){.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}, NULL,
      &state.stagingFence);

  CreateBuffer(device, physicalDevice, sizeof(CameraState),
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
               VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
               &state.cameraBuffer, &state.cameraMemory);
  printf("%d\n", vkMapMemory(device, state.cameraMemory, 0, VK_WHOLE_SIZE, 0,
                             (void **)&state.camera));
  CreateBuffer(device, physicalDevice, sizeof(InputState),
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
               VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
               &state.inputBuffer, &state.inputMemory);
  printf("%d\n", vkMapMemory(device, state.inputMemory, 0, VK_WHOLE_SIZE, 0,
                             (void **)&state.input));
	memset(state.input->selectionBuffer, 0, sizeof(state.input->selectionBuffer));
	memset(state.input->selectionMap, 0, sizeof(state.input->selectionMap));

  glm_mat4_identity_array(&state.camera->model, 3);
  CreateRenderState(&state);
  return state;
}

void ReadInputData(GraphicsState *state) {
  if (!state->inputReadCommandBuffer) {
    CreateBuffer(state->device, state->physicalDevice, sizeof(InputState),
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 &state->stagingInputBuffer, &state->stagingInputMemory);
    vkCreateFence(
        state->device,
        &(VkFenceCreateInfo){.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO},
        NULL, &state->inputReadFence);
    vkAllocateCommandBuffers(
        state->device,
        &(VkCommandBufferAllocateInfo){
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = state->commandPool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1},
        &state->entitySyncCommandBuffer);

    vkBeginCommandBuffer(
        state->inputReadCommandBuffer,
        &(VkCommandBufferBeginInfo){
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO});
    vkCmdCopyBuffer(state->inputReadCommandBuffer, state->inputBuffer,
                    state->stagingInputBuffer, 1,
                    &(VkBufferCopy){.srcOffset = 0,
                                    .dstOffset = 0,
                                    .size = sizeof(InputState)});
    vkEndCommandBuffer(state->inputReadCommandBuffer);
  }

  VkQueue queue;
  vkGetDeviceQueue(
      state->device,
      getQueuesMatching(state->physicalDevice, VK_QUEUE_TRANSFER_BIT, 0)[0], 0,
      &queue);
  vkQueueSubmit(
      queue, 1,
      &(VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                      .commandBufferCount = 1,
                      .pCommandBuffers = &state->inputReadCommandBuffer},
      state->inputReadFence);
  vkWaitForFences(state->device, 1, &state->inputReadFence, 1, 10000);
  vkResetFences(state->device, 1, &state->inputReadFence);
  InputState *dat;
  vkMapMemory(state->device, state->stagingInputMemory, 0, sizeof(InputState),
              0, (void **)&dat);
  memcpy(&state->input, dat, sizeof(InputState));
  vkUnmapMemory(state->device, state->stagingInputMemory);
}

void MoveCamera(GraphicsState *state) {
#define CAMERA_MOVE_SPEED 0.002
#define CAMERA_ROTATE_SPEED 0.01
  static double posx, posy;
  if (state->camera->cameraTurning) {
    double deltax, deltay;
    glfwGetCursorPos(state->window, &deltax, &deltay);
    deltax -= posx;
    deltay -= posy;
    glm_rotate(state->camera->view, deltax * CAMERA_ROTATE_SPEED,
               (vec3){0, 1, 0});
    glm_rotate(state->camera->view, -deltay * CAMERA_ROTATE_SPEED,
               (vec3){1, 0, 0});
    glfwGetCursorPos(state->window, &posx, &posy);
    if (!glfwGetMouseButton(state->window, GLFW_MOUSE_BUTTON_RIGHT)) {
      state->camera->cameraTurning = false;
      glfwSetInputMode(state->window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
      glfwSetInputMode(state->window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
    }
  } else {
    if (glfwGetMouseButton(state->window, GLFW_MOUSE_BUTTON_RIGHT)) {
      state->camera->cameraTurning = true;
      glfwSetInputMode(state->window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
      glfwSetInputMode(state->window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
      glfwGetCursorPos(state->window, &posx, &posy);
    }
  }
  vec4 cameraVelocity = {
      glfwGetKey(state->window, GLFW_KEY_A) * CAMERA_MOVE_SPEED -
          glfwGetKey(state->window, GLFW_KEY_D) * CAMERA_MOVE_SPEED,
      0.,
      glfwGetKey(state->window, GLFW_KEY_S) * CAMERA_MOVE_SPEED -
          glfwGetKey(state->window, GLFW_KEY_W) * CAMERA_MOVE_SPEED,
      1};
  glm_translate(state->camera->view, cameraVelocity);
}

void DrawGraphics(GraphicsState *state) {
  UpdateInputState(state);
  UpdateGraphicsMemory(state);
  if (state->commandBufferDirty) {
    VkSurfaceCapabilitiesKHR capabilites;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(state->physicalDevice,
                                              state->surface, &capabilites);
    for (uint32_t i = 0; i < state->imageCount; i++) {
      vkResetCommandBuffer(state->commandbuffers[i], 0);
      SetupCommandBuffer(state, i, state->entities, state->entityCount);
    }
    state->commandBufferDirty = false;
  }
  state->imageId = (state->imageId + 1) % (state->imageCount + 1);

  uint32_t imageId;
  VkResult result = vkAcquireNextImageKHR(
      state->device, state->swapchain, 1e8,
      state->imageReadySemaphores[state->imageId], VK_NULL_HANDLE, &imageId);
  VkQueue queue;
  vkGetDeviceQueue(
      state->device,
      getQueuesMatching(state->physicalDevice, VK_QUEUE_GRAPHICS_BIT, 0)[0], 0,
      &queue);
  if (result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR) {
    printf("Swapchain unsuitable, recreating..\n");
    vkQueueWaitIdle(queue);
    CleanRenderState(state);
    CreateRenderState(state);
    return;
  }
  if (result == VK_TIMEOUT) {
    printf("Device not ready yet, try again later\n");
    return;
  }
  if (result != VK_SUCCESS) {
    printf("Failure to fetch image err: %d.. Returning\n", result);
    return;
  }

  vkWaitForFences(state->device, 1, &state->imageReadyFences[imageId], 1,
                  100000);
  vkResetFences(state->device, 1, &state->imageReadyFences[imageId]);
  vkQueueSubmit(
      queue, 1,
      &(VkSubmitInfo){
          .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
          .commandBufferCount = 1,
          .pCommandBuffers = &state->commandbuffers[imageId],
          .waitSemaphoreCount = 1,
          .pWaitSemaphores = &state->imageReadySemaphores[state->imageId],
          .signalSemaphoreCount = 1,
          .pSignalSemaphores = &state->renderFinishedSemaphores[imageId],
          .pWaitDstStageMask =
              &(VkPipelineStageFlags){
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT}},
      state->imageReadyFences[imageId]);
  vkQueuePresentKHR(
      queue, &(VkPresentInfoKHR){.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                 .swapchainCount = 1,
                                 .waitSemaphoreCount = 1,
                                 .pWaitSemaphores =
                                     &state->renderFinishedSemaphores[imageId],
                                 .pSwapchains = &state->swapchain,
                                 .pImageIndices = &imageId});
}
