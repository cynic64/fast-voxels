#include <vulkan/vulkan_core.h>
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <vulkan/vulkan.h>

#include <cglm/cglm.h>

#include "external/render-c/src/base.h"
#include "external/render-c/src/buffer.h"
#include "external/render-c/src/glfw_error.h"
#include "external/render-c/src/image.h"
#include "external/render-c/src/pipeline.h"
#include "external/render-c/src/rpass.h"
#include "external/render-c/src/set.h"
#include "external/render-c/src/shader.h"
#include "external/render-c/src/swapchain.h"
#include "external/render-c/src/sync.h"

#include "external/render-utils/src/camera.h"
#include "external/render-utils/src/timer.h"

#include <assert.h>

const char* DEVICE_EXTS[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
const int DEVICE_EXT_CT = 1;

const double MOUSE_SENSITIVITY_FACTOR = 0.001;
const float MOVEMENT_SPEED = 0.6F;

#define CONCURRENT_FRAMES 4

const VkFormat SC_FORMAT_PREF = VK_FORMAT_B8G8R8A8_UNORM;
const VkPresentModeKHR SC_PRESENT_MODE_PREF = VK_PRESENT_MODE_IMMEDIATE_KHR;

struct SyncSet {
        VkFence render_fence;
        VkSemaphore acquire_sem;
        VkSemaphore render_sem;
};

struct PushConstants {
	// Pad to vec4 because std140
	vec4 iResolution;
	vec4 iMouse;
	float iFrame[1];
	float iTime[1];
	int32_t tex_width;
};

void sync_set_create(VkDevice device, struct SyncSet* sync_set) {
        fence_create(device, VK_FENCE_CREATE_SIGNALED_BIT, &sync_set->render_fence);
        semaphore_create(device, &sync_set->acquire_sem);
        semaphore_create(device, &sync_set->render_sem);
}

void sync_set_destroy(VkDevice device, struct SyncSet* sync_set) {
	vkDestroyFence(device, sync_set->render_fence, NULL);
	vkDestroySemaphore(device, sync_set->acquire_sem, NULL);
	vkDestroySemaphore(device, sync_set->render_sem, NULL);
}

int main() {
        glfwInit();
        glfwSetErrorCallback(glfw_error_callback);
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        GLFWwindow* window = glfwCreateWindow(800, 600, "Vulkan", NULL, NULL);
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

	// Base
        struct Base base;
        base_create(window, 1, 0, NULL, DEVICE_EXT_CT, DEVICE_EXTS, &base);

	// Swapchain
        struct Swapchain swapchain;
        swapchain_create(base.surface, base.phys_dev, base.device, SC_FORMAT_PREF, SC_PRESENT_MODE_PREF, &swapchain);

        // Load shaders
        VkShaderModule vs = load_shader(base.device, "shaders/shader.vs.spv");
        VkShaderModule fs = load_shader(base.device, "shaders/shader.fs.spv");

        VkPipelineShaderStageCreateInfo shaders[2] = {0};
        shaders[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shaders[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        shaders[0].module = vs;
        shaders[0].pName = "main";
        shaders[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shaders[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        shaders[1].module = fs;
        shaders[1].pName = "main";

        // Render pass
	VkRenderPass rpass;
	rpass_color(base.device, swapchain.format, &rpass);

        // Framebuffers
        VkFramebuffer* framebuffers = malloc(swapchain.image_ct * sizeof(framebuffers[0]));
        for (int i = 0; i < swapchain.image_ct; i++) {
                VkImageView views[] = {swapchain.views[i]};
                framebuffer_create(base.device, rpass, swapchain.width, swapchain.height,
                                   sizeof(views) / sizeof(views[0]), views, &framebuffers[i]);
        }

        // Command buffers
        VkCommandBuffer cbufs[CONCURRENT_FRAMES];
        for (int i = 0; i < CONCURRENT_FRAMES; i++) cbuf_alloc(base.device, base.cpool, &cbufs[i]);

        // Sync sets
        struct SyncSet sync_sets [CONCURRENT_FRAMES];
        for (int i = 0; i < CONCURRENT_FRAMES; i++) sync_set_create(base.device, &sync_sets[i]);

        // Image fences
        VkFence* image_fences = malloc(swapchain.image_ct * sizeof(image_fences[0]));
        for (int i = 0; i < swapchain.image_ct; i++) image_fences[i] = VK_NULL_HANDLE;

	// Texture for shader
	const int tex_width = 1024;
	// Using 1 byte per voxel
	const int image_size = tex_width * tex_width;
	struct Buffer tex_buf;
	buffer_create(base.phys_dev, base.device, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		      image_size, &tex_buf);

	// Write some data
	unsigned char *tex_buf_mapped;
	vkMapMemory(base.device, tex_buf.mem, 0, tex_buf.size, 0, (void **) &tex_buf_mapped);
	for (int i = 0; i < tex_width; i++) {
		for (int j = 0; j < tex_width; j++) {
			tex_buf_mapped[i * tex_width + j] = j % 2 == 0 ? 0x7f : 0x00;
		}
	}

	vkUnmapMemory(base.device, tex_buf.mem);

	// Create the actual image
	struct Image tex;
	image_create(base.phys_dev, base.device, VK_FORMAT_R8_UNORM, tex_width, tex_width,
		     VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
		     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		     VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT,
		     1, VK_SAMPLE_COUNT_1_BIT, &tex);

	// Copy buffer to image
	image_trans(base.device, base.queue, base.cpool, tex.handle,
		    VK_IMAGE_ASPECT_COLOR_BIT,
		    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    VK_ACCESS_NONE, VK_ACCESS_TRANSFER_WRITE_BIT,
		    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 1);

	image_copy_from_buffer(base.device, base.queue, base.cpool, VK_IMAGE_ASPECT_COLOR_BIT,
			       tex_buf.handle, tex.handle, tex_width, tex_width);

	image_trans(base.device, base.queue, base.cpool, tex.handle,
		    VK_IMAGE_ASPECT_COLOR_BIT,
		    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
		    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 1);

	// Create sampler
	VkSamplerCreateInfo sampler_info = {0};
	sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler_info.magFilter = VK_FILTER_NEAREST;
	sampler_info.minFilter = VK_FILTER_NEAREST;
	sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.unnormalizedCoordinates = VK_TRUE;
	sampler_info.compareEnable = VK_FALSE;

	VkSampler tex_sampler;
	VkResult res = vkCreateSampler(base.device, &sampler_info, NULL, &tex_sampler);
	assert(res == VK_SUCCESS);

	// Create descriptors
	VkDescriptorImageInfo set_desc_img = {0};
	set_desc_img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	set_desc_img.imageView = tex.view;
	set_desc_img.sampler = tex_sampler;

	struct Descriptor set_desc = {0};
	set_desc.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	set_desc.binding = 0;
	set_desc.shader_stage_flags = VK_SHADER_STAGE_FRAGMENT_BIT;
	set_desc.image = set_desc_img;

	// Create descriptor pool
	VkDescriptorPoolSize dpool_size = {0};
	dpool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	dpool_size.descriptorCount = 1;

	VkDescriptorPoolCreateInfo dpool_info = {0};
	dpool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	dpool_info.maxSets = CONCURRENT_FRAMES;
	dpool_info.poolSizeCount = 1;
	dpool_info.pPoolSizes = &dpool_size;

	VkDescriptorPool dpool;
	res = vkCreateDescriptorPool(base.device, &dpool_info, NULL, &dpool);
	assert(res == VK_SUCCESS);

	VkDescriptorSetLayoutBinding set_binding = {0};
	set_binding.binding = 0;
	set_binding.descriptorCount = 1;
	set_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	set_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	VkDescriptorSetLayout set_layout;
	set_layout_create(base.device, 1, &set_binding, &set_layout);

	// Finally create the set
	VkDescriptorSet set;
	set_create(base.device, dpool, set_layout, 1, &set_desc, &set);

        // Pipeline layout
	VkPushConstantRange pushc_range = {0};
        pushc_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushc_range.offset = 0;
        pushc_range.size = sizeof(struct PushConstants);

        VkPipelineLayoutCreateInfo pipeline_layout_info = {0};
        pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_info.pushConstantRangeCount = 1;
        pipeline_layout_info.pPushConstantRanges = &pushc_range;
	pipeline_layout_info.setLayoutCount = 1;
	pipeline_layout_info.pSetLayouts = &set_layout;

        VkPipelineLayout pipeline_layout;
        res = vkCreatePipelineLayout(base.device, &pipeline_layout_info, NULL,
					      &pipeline_layout);
        assert(res == VK_SUCCESS);

        // Pipeline
        struct PipelineSettings pipeline_settings = PIPELINE_SETTINGS_DEFAULT;

	VkPipeline pipeline;
	pipeline_create(base.device, &pipeline_settings,
	                sizeof(shaders) / sizeof(shaders[0]), shaders,
	                pipeline_layout, rpass, 0, &pipeline);

        vkDestroyShaderModule(base.device, vs, NULL);
        vkDestroyShaderModule(base.device, fs, NULL);

	// Camera
	struct CameraFly camera;
	camera.pitch = 0.0F;
	camera.yaw = 0.0F;
	camera.eye[0] = 0.0F; camera.eye[1] = 0.0F; camera.eye[2] = 0.0F; 
	double last_mouse_x, last_mouse_y;
	glfwGetCursorPos(window, &last_mouse_x, &last_mouse_y);

	// Exponent for SDF
	float sdf_exp = 1.0F;

	// Main loop
        int frame_ct = 0;
        struct timespec start_time = timer_start();
        struct timespec last_frame_time = timer_start();

        int must_recreate = 0;
        while (!glfwWindowShouldClose(window)) {
                while (must_recreate) {
                        must_recreate = 0;
                        vkDeviceWaitIdle(base.device);

                        VkFormat old_format = swapchain.format;
                        uint32_t old_image_ct = swapchain.image_ct;

                        swapchain_destroy(base.device, &swapchain);
                        swapchain_create(base.surface, base.phys_dev, base.device,
                                         old_format, SC_PRESENT_MODE_PREF, &swapchain);

                        assert(swapchain.format == old_format && swapchain.image_ct == old_image_ct);

                        for (int i = 0; i < CONCURRENT_FRAMES; i++) {
                                sync_set_destroy(base.device, &sync_sets[i]);
                                sync_set_create(base.device, &sync_sets[i]);
                        }

                        for (int i = 0; i < swapchain.image_ct; i++) {
                                vkDestroyFramebuffer(base.device, framebuffers[i], NULL);

                                VkImageView views[] = {swapchain.views[i]};
                                framebuffer_create(base.device, rpass, swapchain.width, swapchain.height,
                                                   sizeof(views) / sizeof(views[0]), views, &framebuffers[i]);

                                image_fences[i] = VK_NULL_HANDLE;
                        }

                        int real_width, real_height;
                        glfwGetFramebufferSize(window, &real_width, &real_height);
                        if (real_width != swapchain.width || real_height != swapchain.height) must_recreate = 1;
                }

                // Handle input
                // Mouse movement
                double new_mouse_x, new_mouse_y;
                glfwGetCursorPos(window, &new_mouse_x, &new_mouse_y);
                double d_mouse_x = new_mouse_x - last_mouse_x, d_mouse_y = new_mouse_y - last_mouse_y;
                double delta = timer_get_elapsed(&last_frame_time);
                last_frame_time = timer_start(last_frame_time);
        	last_mouse_x = new_mouse_x; last_mouse_y = new_mouse_y;

        	// Keys
        	vec3 cam_movement = {0.0F, 0.0F, 0.0F};
        	if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) cam_movement[2] += MOVEMENT_SPEED;
        	if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) cam_movement[2] -= MOVEMENT_SPEED;
        	if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) cam_movement[0] -= MOVEMENT_SPEED;
        	if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) cam_movement[0] += MOVEMENT_SPEED;

        	if (glfwGetKey(window, GLFW_KEY_Z) == GLFW_PRESS) sdf_exp += delta * 0.2;
        	if (glfwGetKey(window, GLFW_KEY_X) == GLFW_PRESS) sdf_exp -= delta * 0.2;

        	// Update camera
        	camera_fly_update(&camera,
                                  d_mouse_x * MOUSE_SENSITIVITY_FACTOR, d_mouse_y * MOUSE_SENSITIVITY_FACTOR,
        	                  cam_movement, delta);

		// Set up frame
                int frame_idx = frame_ct % CONCURRENT_FRAMES;
                struct SyncSet* sync_set = &sync_sets[frame_idx];

                VkCommandBuffer cbuf = cbufs[frame_idx];

                // Wait for the render process using these sync objects to finish rendering
                res = vkWaitForFences(base.device, 1, &sync_set->render_fence, VK_TRUE, UINT64_MAX);
                assert(res == VK_SUCCESS);

                // Reset command buffer
                vkResetCommandBuffer(cbuf, 0);

                // Acquire an image
                uint32_t image_idx;
                res = vkAcquireNextImageKHR(base.device, swapchain.handle, UINT64_MAX,
                                            sync_set->acquire_sem, VK_NULL_HANDLE, &image_idx);
                if (res == VK_ERROR_OUT_OF_DATE_KHR) {
                        must_recreate = 1;
                        continue;
                } else assert(res == VK_SUCCESS);

                // Record command buffer

                cbuf_begin_onetime(cbuf);

                VkClearValue clear_vals[] = {{{{0.0F, 0.0F, 0.0F, 1.0F}}},
                                             {{{0.0F, 0.0F, 0.0F, 1.0F}}}};

                VkRenderPassBeginInfo cbuf_rpass_info = {0};
                cbuf_rpass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                cbuf_rpass_info.renderPass = rpass;
                cbuf_rpass_info.framebuffer = framebuffers[image_idx];
                cbuf_rpass_info.renderArea.extent.width = swapchain.width;
                cbuf_rpass_info.renderArea.extent.height = swapchain.height;
                cbuf_rpass_info.clearValueCount = sizeof(clear_vals) / sizeof(clear_vals[0]);
                cbuf_rpass_info.pClearValues = clear_vals;
                vkCmdBeginRenderPass(cbuf, &cbuf_rpass_info, VK_SUBPASS_CONTENTS_INLINE);

                vkCmdBindPipeline(cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

                VkViewport viewport = {0};
                viewport.width = swapchain.width;
                viewport.height = swapchain.height;
                viewport.minDepth = 0.0F;
                viewport.maxDepth = 1.0F;
                vkCmdSetViewport(cbuf, 0, 1, &viewport);

                VkRect2D scissor = {0};
                scissor.extent.width = swapchain.width;
                scissor.extent.height = swapchain.height;
                vkCmdSetScissor(cbuf, 0, 1, &scissor);

                struct PushConstants pushc_data;
		pushc_data.iResolution[0] = swapchain.width;
		pushc_data.iResolution[1] = swapchain.height;
		pushc_data.iMouse[0] = new_mouse_x;
		pushc_data.iMouse[1] = new_mouse_y;
		pushc_data.iMouse[2] = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
		pushc_data.iFrame[0] = frame_ct;
		pushc_data.iTime[0] = timer_get_elapsed(&start_time);
		pushc_data.tex_width = tex_width;

		vkCmdPushConstants(cbuf, pipeline_layout,
				   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		                   0, sizeof(struct PushConstants), &pushc_data);
		vkCmdBindDescriptorSets(cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1,
					&set, 0, NULL);
                vkCmdDraw(cbuf, 6, 1, 0, 0);

                vkCmdEndRenderPass(cbuf);

                res = vkEndCommandBuffer(cbuf);
                assert(res == VK_SUCCESS);

                // Wait until whoever is rendering to the image is done
                if (image_fences[image_idx] != VK_NULL_HANDLE)
                        vkWaitForFences(base.device, 1, &image_fences[image_idx], VK_TRUE, UINT64_MAX);

                // Reset fence
                res = vkResetFences(base.device, 1, &sync_set->render_fence);
                assert(res == VK_SUCCESS);

                // Mark it as in use by us
                image_fences[image_idx] = sync_set->render_fence;

                // Submit
                VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                VkSubmitInfo submit_info = {0};
                submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submit_info.waitSemaphoreCount = 1;
                submit_info.pWaitSemaphores = &sync_set->acquire_sem;
                submit_info.pWaitDstStageMask = &wait_stage;
                submit_info.commandBufferCount = 1;
                submit_info.pCommandBuffers = &cbuf;
                submit_info.signalSemaphoreCount = 1;
                submit_info.pSignalSemaphores = &sync_set->render_sem;

                res = vkQueueSubmit(base.queue, 1, &submit_info, sync_set->render_fence);
                assert(res == VK_SUCCESS);

                // Present
                VkPresentInfoKHR present_info = {0};
                present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
                present_info.waitSemaphoreCount = 1;
                present_info.pWaitSemaphores = &sync_set->render_sem;
                present_info.swapchainCount = 1;
                present_info.pSwapchains = &swapchain.handle;
                present_info.pImageIndices = &image_idx;

                res = vkQueuePresentKHR(base.queue, &present_info);
                if (res == VK_ERROR_OUT_OF_DATE_KHR) must_recreate = 1;
                else assert(res == VK_SUCCESS);

                frame_ct++;
                glfwPollEvents();
        }

	double elapsed = timer_get_elapsed(&start_time);
        double fps = (double) frame_ct / elapsed;
        printf("FPS: %.2f\n", fps);

        vkDeviceWaitIdle(base.device);

        vkDestroyPipeline(base.device, pipeline, NULL);
        vkDestroyPipelineLayout(base.device, pipeline_layout, NULL);

        vkDestroyRenderPass(base.device, rpass, NULL);

        swapchain_destroy(base.device, &swapchain);

        for (int i = 0; i < CONCURRENT_FRAMES; i++) {
                sync_set_destroy(base.device, &sync_sets[i]);
        }

        for (int i = 0; i < swapchain.image_ct; i++) {
                vkDestroyFramebuffer(base.device, framebuffers[i], NULL);
        }

	vkDestroyDescriptorSetLayout(base.device, set_layout, NULL);
	vkDestroyDescriptorPool(base.device, dpool, NULL);
	vkDestroySampler(base.device, tex_sampler, NULL);
	image_destroy(base.device, &tex);
	buffer_destroy(base.device, &tex_buf);

        base_destroy(&base);

        glfwTerminate();

        free(framebuffers);
        free(image_fences);
}
