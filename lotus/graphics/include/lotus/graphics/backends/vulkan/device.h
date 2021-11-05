#pragma once

/// \file
/// Devices.

#include <optional>

#include "commands.h"
#include "descriptors.h"
#include "details.h"
#include "frame_buffer.h"

namespace lotus::graphics::backends::vulkan {
	class adapter;
	class command_list;
	class context;
	class device;


	/// Contains a \p vk::UniqueDevice.
	class device {
		friend adapter;
		friend context;
		friend void command_allocator::reset(device&);
	protected:
		/// Creates an empty object.
		device(std::nullptr_t) {
		}

		/// Calls \p vk::UniqueDevice::acquireNextImageKHR().
		[[nodiscard]] back_buffer_info acquire_back_buffer(swap_chain&);

		/// Calls \p vk::UniqueDevice::getQueue();
		[[nodiscard]] command_queue create_command_queue();
		/// Calls \p vk::UniqueDevice::createCommandPoolUnique().
		[[nodiscard]] command_allocator create_command_allocator();
		/// Calls \p vk::UniqueDevice::allocateCommandBuffersUnique() and \p vk::UniqueCommandBuffer::begin().
		[[nodiscard]] command_list create_and_start_command_list(command_allocator&);

		/// Calls \p vk::UniqueDevice::createDescriptorPoolUnique().
		[[nodiscard]] descriptor_pool create_descriptor_pool(
			std::span<const descriptor_range> capacity, std::size_t max_num_sets
		);
		/// Calls \p vk::UniqueDevice::allocateDescriptorSetsUnique().
		[[nodiscard]] descriptor_set create_descriptor_set(
			descriptor_pool&, const descriptor_set_layout&
		);

		/// Calls \p vk::UniqueDevice::updateDescriptorSets().
		void write_descriptor_set_images(
			descriptor_set&, const descriptor_set_layout&,
			std::size_t first_register, std::span<const image_view *const>
		);
		/// Calls \p vk::UniqueDevice::updateDescriptorSets().
		void write_descriptor_set_buffers(
			descriptor_set&, const descriptor_set_layout&,
			std::size_t first_register, std::span<const buffer_view>
		);
		/// Calls \p vk::UniqueDevice::updateDescriptorSets().
		void write_descriptor_set_constant_buffers(
			descriptor_set&, const descriptor_set_layout&,
			std::size_t first_register, std::span<const constant_buffer_view>
		);
		/// Calls \p vk::UniqueDevice::updateDescriptorSets().
		void write_descriptor_set_samplers(
			descriptor_set &set, const descriptor_set_layout &layout,
			std::size_t first_register, std::span<const graphics::sampler *const> samplers
		);

		/// Calls \p vk::UniqueDevice::createShaderModuleUnique().
		[[nodiscard]] shader load_shader(std::span<const std::byte>);
		/// \todo
		[[nodiscard]] shader_reflection load_shader_reflection(std::span<const std::byte>);

		/// Calls \p vk::UniqueDevice::createSamplerUnique().
		[[nodiscard]] sampler create_sampler(
			filtering minification, filtering magnification, filtering mipmapping,
			float mip_lod_bias, float min_lod, float max_lod, std::optional<float> max_anisotropy,
			sampler_address_mode addressing_u, sampler_address_mode addressing_v, sampler_address_mode addressing_w,
			linear_rgba_f border_color, std::optional<comparison_function> comparison
		);

		/// Calls \p vk::UniqueDevice::createDescriptorSetLayoutUnique().
		[[nodiscard]] descriptor_set_layout create_descriptor_set_layout(
			std::span<const descriptor_range_binding>, shader_stage visible_stages
		);

		/// Calls \p vk::UniqueDevice::createPipelineLayoutUnique().
		[[nodiscard]] pipeline_resources create_pipeline_resources(
			std::span<const graphics::descriptor_set_layout *const>
		);
		/// Calls \p vk::UniqueDevice::createGraphicsPipelineUnique().
		[[nodiscard]] graphics_pipeline_state create_graphics_pipeline_state(
			const pipeline_resources&,
			const shader *vs,
			const shader *ps,
			const shader *ds,
			const shader *hs,
			const shader *gs,
			std::span<const render_target_blend_options>,
			const rasterizer_options&,
			const depth_stencil_options&,
			std::span<const input_buffer_layout>,
			primitive_topology,
			const pass_resources&,
			std::size_t num_viewports = 1
		);
		/// Calls \p vk::UniqueDevice::createComputePipelineUnique().
		[[nodiscard]] compute_pipeline_state create_compute_pipeline_state(
			const pipeline_resources&, const shader&
		);

		/// Calls \p vk::UniqueDevice::createRenderPassUnique().
		[[nodiscard]] pass_resources create_pass_resources(
			std::span<const render_target_pass_options>,
			depth_stencil_pass_options
		);

		/// Calls \p vk::UniqueDevice::allocateMemoryUnique().
		[[nodiscard]] device_heap create_device_heap(std::size_t size, heap_type);

		/// Calls \p vk::UniqueDevice::createBuffer() to create the buffer, then calls
		/// \p vk::UniqueDevice::allocateMemory() to allocate memory for it.
		[[nodiscard]] buffer create_committed_buffer(
			std::size_t size, heap_type, buffer_usage::mask allowed_usage
		);
		/// Calls \p vk::UniqueDevice::createImage() to create the image, then calls
		/// \p vk::UniqueDevice::allocateMemory() to allocate memory for it.
		[[nodiscard]] image2d create_committed_image2d(
			std::size_t width, std::size_t height, std::size_t array_slices, std::size_t mip_levels,
			format, image_tiling, image_usage::mask allowed_usage
		);
		/// Obtains the layout of the buffer by creating a dummy image object, then calls
		/// \ref create_committed_buffer() to create the buffer.
		[[nodiscard]] std::tuple<buffer, staging_buffer_pitch, std::size_t> create_committed_staging_buffer(
			std::size_t width, std::size_t height, format, heap_type,
			buffer_usage::mask allowed_usage
		);

		/// Calls \ref _map_memory().
		[[nodiscard]] void *map_buffer(buffer&, std::size_t begin, std::size_t length);
		/// Calls \ref _unmap_memory().
		void unmap_buffer(buffer&, std::size_t begin, std::size_t length);
		/// Calls \ref _map_memory().
		[[nodiscard]] void *map_image2d(
			image2d&, subresource_index, std::size_t begin, std::size_t length
		);
		/// Calls \ref _unmap_memory().
		void unmap_image2d(
			image2d&, subresource_index, std::size_t begin, std::size_t length
		);

		/// Calls \p vk::UniqueDevice::createImageViewUnique().
		[[nodiscard]] image2d_view create_image2d_view_from(const image2d&, format, mip_levels);
		/// Calls \p vk::UniqueDevice::createFramebufferUnique().
		[[nodiscard]] frame_buffer create_frame_buffer(
			std::span<const graphics::image2d_view *const> color, const image2d_view *depth_stencil,
			const cvec2s &size, const pass_resources&
		);

		/// Calls \p vk::UniqueDevice::createFenceUnique().
		[[nodiscard]] fence create_fence(synchronization_state state);

		/// Calls \p vk::UniqueDevice::resetFences().
		void reset_fence(fence&);
		/// Calls \p vk::UniqueDevice::waitForFences().
		void wait_for_fence(fence&);

		/// Calls \p vk::UniqueDevice::debugMarkerSetObjectNameEXT().
		void set_debug_name(buffer&, const char8_t*);
		/// Calls \p vk::UniqueDevice::debugMarkerSetObjectNameEXT().
		void set_debug_name(image&, const char8_t*);
	private:
		vk::UniqueDevice _device; ///< The device.
		vk::PhysicalDevice _physical_device; ///< The physical device.
		// queue indices
		std::uint32_t _graphics_compute_queue_family_index; ///< Graphics and compute command queue family index.
		std::uint32_t _compute_queue_family_index; ///< Compute-only command queue family index.
		vk::PhysicalDeviceLimits _device_limits; ///< Device limits.
		vk::PhysicalDeviceMemoryProperties _memory_properties; ///< Memory properties.
		const vk::DispatchLoaderDynamic *_dispatch_loader; ///< The dispatch loader.

		/// Finds the best memory type fit for the given requirements and \ref heap_type.
		[[nodiscard]] std::uint32_t _find_memory_type_index(std::uint32_t requirements, heap_type) const;
		/// Finds the best memory type fit for the given requirements and memory flags.
		[[nodiscard]] std::uint32_t _find_memory_type_index(
			std::uint32_t requirements,
			vk::MemoryPropertyFlags required_on, vk::MemoryPropertyFlags required_off,
			vk::MemoryPropertyFlags optional_on, vk::MemoryPropertyFlags optional_off
		) const;

		/// Maps the given memory, and invalidates the given memory range.
		[[nodiscard]] void *_map_memory(vk::DeviceMemory, std::size_t beg, std::size_t len);
		/// Unmaps the given memory, and flushes the given memory range.
		void _unmap_memory(vk::DeviceMemory, std::size_t beg, std::size_t len);
	};

	/// Contains a \p vk::PhysicalDevice.
	class adapter {
		friend context;
	protected:
		/// Creates an empty object.
		adapter(std::nullptr_t) {
		}

		/// Enumerates all queue families using \p vk::PhysicalDevice::getQueueFamilyProperties(), then creates a
		/// device using \p vk::PhysicalDevice::createDeviceUnique().
		[[nodiscard]] device create_device();
		/// Returns the results of \p vk::PhysicalDevice::getProperties().
		[[nodiscard]] adapter_properties get_properties() const;
	private:
		vk::PhysicalDevice _device; ///< The physical device.
		const vk::DispatchLoaderDynamic *_dispatch_loader; ///< Dispatch loader.

		/// Initializes all fields of the struct.
		adapter(vk::PhysicalDevice dev, const vk::DispatchLoaderDynamic &dispatch) :
			_device(dev), _dispatch_loader(&dispatch) {
		}
	};
}