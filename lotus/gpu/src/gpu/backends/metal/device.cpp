#include "lotus/gpu/backends/metal/device.h"

/// \file
/// Implementation of Metal devices.

#include <array>

#include <QuartzCore/QuartzCore.hpp>
#include <Metal/Metal.hpp>

#include "lotus/gpu/resources.h"
#include "lotus/gpu/backends/common/dxc.h"
#include "metal_irconverter_include.h"

namespace lotus::gpu::backends::metal {
	back_buffer_info device::acquire_back_buffer(swap_chain &chain) {
		chain._drawable = NS::RetainPtr(chain._layer->nextDrawable());
		if (!chain._drawable) {
			return nullptr;
		}
		back_buffer_info result = nullptr;
		result.index  = 0;
		result.status = swap_chain_status::ok;
		return result;
	}

	void device::resize_swap_chain_buffers(swap_chain &chain, cvec2u32 size) {
		chain._layer->setDrawableSize(CGSizeMake(size[0], size[1]));
	}

	command_allocator device::create_command_allocator(command_queue &q) {
		return command_allocator(q._q.get());
	}

	command_list device::create_and_start_command_list(command_allocator &alloc) {
		return command_list(NS::RetainPtr(alloc._q->commandBuffer()));
	}

	/// Default resource options for argument buffers. Assumes that we don't need to read from the argument buffer.
	constexpr static MTL::ResourceOptions _arg_buffer_options =
		MTL::ResourceOptionCPUCacheModeWriteCombined |
		MTL::ResourceStorageModeShared |
		MTL::ResourceHazardTrackingModeUntracked;

	descriptor_pool device::create_descriptor_pool(
		std::span<const descriptor_range> capacity, std::size_t max_num_sets
	) {
		std::size_t total_resources = 0;
		for (const descriptor_range &range : capacity) {
			total_resources += range.count;
		}
		auto heap_desc = NS::TransferPtr(MTL::HeapDescriptor::alloc()->init());
		heap_desc->setType(MTL::HeapTypeAutomatic);
		heap_desc->setResourceOptions(_arg_buffer_options);
		heap_desc->setSize(total_resources * sizeof(IRDescriptorTableEntry));
		auto heap = NS::TransferPtr(_dev->newHeap(heap_desc.get()));
		return descriptor_pool(std::move(heap));
	}

	descriptor_set device::create_descriptor_set(descriptor_pool &pool, const descriptor_set_layout &layout) {
		std::uint32_t max_slot_index = 0;
		for (const descriptor_range_binding &range : layout._bindings) {
			max_slot_index = std::max(max_slot_index, range.get_last_register_index());
		}
		const NS::UInteger size_bytes = (max_slot_index + 1) * sizeof(IRDescriptorTableEntry);
		auto buf_ptr = NS::TransferPtr(pool._heap->newBuffer(size_bytes, _arg_buffer_options));
		return descriptor_set(std::move(buf_ptr), max_slot_index + 1);
	}

	descriptor_set device::create_descriptor_set(
		descriptor_pool &pool, const descriptor_set_layout &layout, std::size_t dynamic_size
	) {
		std::uint32_t max_slot_index = 0;
		for (const descriptor_range_binding &range : layout._bindings) {
			if (range.range.count == descriptor_range::unbounded_count) {
				max_slot_index = range.register_index + dynamic_size - 1;
			} else {
				max_slot_index = std::max(max_slot_index, range.get_last_register_index());
			}
		}
		const NS::UInteger size_bytes = (max_slot_index + 1) * sizeof(IRDescriptorTableEntry);
		auto buf_ptr = NS::TransferPtr(pool._heap->newBuffer(size_bytes, _arg_buffer_options));
		return descriptor_set(std::move(buf_ptr), max_slot_index + 1);
	}

	void device::write_descriptor_set_read_only_images(
		descriptor_set &set,
		const descriptor_set_layout &layout,
		std::size_t first_register,
		std::span<const image_view_base *const> images
	) {
		// TODO validate that we're writing to a range of the correct type
		auto *arr = static_cast<IRDescriptorTableEntry*>(set._arg_buffer->contents());
		for (std::size_t i = 0; i < images.size(); ++i) {
			const auto *const img = static_cast<const _details::basic_image_view_base*>(images[i]);
			IRDescriptorTableSetTexture(&arr[first_register + i], img->_tex.get(), 0.0f, 0);
			set._resources[first_register + i] = img->_tex;
		}
	}

	void device::write_descriptor_set_read_write_images(
		descriptor_set &set,
		const descriptor_set_layout &layout,
		std::size_t first_register,
		std::span<const image_view_base *const> images
	) {
		// Metal does not distinguish between read-only and read-write bindings
		write_descriptor_set_read_only_images(set, layout, first_register, images);
	}

	void device::write_descriptor_set_read_only_structured_buffers(
		descriptor_set &set,
		const descriptor_set_layout &layout,
		std::size_t first_regsiter,
		std::span<const structured_buffer_view> buffers
	) {
		// TODO validate that we're writing to a range of the correct type
		auto *arr = static_cast<IRDescriptorTableEntry*>(set._arg_buffer->contents());
		for (std::size_t i = 0; i < buffers.size(); ++i) {
			// TODO metal does not support custom strides?
			IRBufferView view = {};
			view.buffer       = buffers[i].data->_buf.get();
			view.bufferOffset = buffers[i].first * buffers[i].stride;
			view.bufferSize   = buffers[i].count * buffers[i].stride;
			view.typedBuffer  = true;
			IRDescriptorTableSetBufferView(&arr[first_regsiter + i], &view);
			set._resources[first_regsiter + i] = buffers[i].data->_buf;
		}
	}

	void device::write_descriptor_set_read_write_structured_buffers(
		descriptor_set &set,
		const descriptor_set_layout &layout,
		std::size_t first_regsiter,
		std::span<const structured_buffer_view> buffers
	) {
		// Metal does not distinguish between read-only and read-write bindings
		write_descriptor_set_read_only_structured_buffers(set, layout, first_regsiter, buffers);
	}

	void device::write_descriptor_set_constant_buffers(
		descriptor_set &set,
		const descriptor_set_layout &layout,
		std::size_t first_register,
		std::span<const constant_buffer_view> buffers
	) {
		// TODO validate that we're writing to a range of the correct type
		auto *arr = static_cast<IRDescriptorTableEntry*>(set._arg_buffer->contents());
		for (std::size_t i = 0; i < buffers.size(); ++i) {
			const std::uint64_t base_addr = buffers[i].data->_buf->gpuAddress();
			IRDescriptorTableSetBuffer(&arr[first_register + i], base_addr + buffers[i].offset, 0);
			set._resources[first_register + i] = buffers[i].data->_buf;
		}
	}

	void device::write_descriptor_set_samplers(
		descriptor_set &set,
		const descriptor_set_layout &layout,
		std::size_t first_register,
		std::span<const gpu::sampler *const> samplers
	) {
		// TODO validate that we're writing to a range of the correct type
		auto *arr = static_cast<IRDescriptorTableEntry*>(set._arg_buffer->contents());
		for (std::size_t i = 0; i < samplers.size(); ++i) {
			IRDescriptorTableSetSampler(
				&arr[first_register + i],
				samplers[i]->_smp.get(),
				samplers[i]->_mip_lod_bias
			);
			set._resources[first_register + i] = nullptr; // no need to call useResource() for samplers
		}
	}

	shader_binary device::load_shader(std::span<const std::byte> data) {
		shader_binary result = nullptr;

		// convert from DXIL to Metal IR
		common::dxc_compiler compiler = nullptr;
		common::dxil_reflection refl = compiler.load_shader_reflection(data);
		const common::dxil_reflection::reflection_ptr_union &refl_ptr = refl.get_raw_ptr();
		crash_if(!std::holds_alternative<common::dxil_reflection::shader_reflection_ptr>(refl_ptr));
		ID3D12ShaderReflection *dx_refl = std::get<common::dxil_reflection::shader_reflection_ptr>(refl_ptr).p;
		_details::ir_unique_ptr<IRRootSignature> root_signature =
			_details::shader::create_root_signature_for_dxil_reflection(dx_refl);
		_details::shader::ir_conversion_result result_ir = _details::shader::convert_to_metal_ir(data, dx_refl);

		// load vertex shader reflection data
		auto shader_refl = _details::ir_make_unique(IRShaderReflectionCreate());
		crash_if(!IRObjectGetReflection(
			result_ir.object.get(),
			IRObjectGetMetalIRShaderStage(result_ir.object.get()),
			shader_refl.get()
		));
		if (
			IRVersionedVSInfo vsinfo;
			IRShaderReflectionCopyVertexInfo(shader_refl.get(), IRReflectionVersion_1_0, &vsinfo)
		) {
			for (std::size_t i = 0; i < vsinfo.info_1_0.num_vertex_inputs; ++i) {
				const IRVertexInputInfo_1_0 &input = vsinfo.info_1_0.vertex_inputs[i];
				result._vs_input_attributes.emplace_back(
					std::u8string(string::assume_utf8(input.name)),
					input.attributeIndex
				);
			}
			IRShaderReflectionReleaseVertexInfo(&vsinfo);
		}

		// load compute shader reflection data
		result._thread_group_size = refl.get_thread_group_size();

		NS::Error *err = nullptr;
		result._lib = NS::TransferPtr(_dev->newLibrary(result_ir.data, &err));
		if (err) {
			log().error("{}", err->localizedDescription()->utf8String());
			std::abort();
		}
		// TODO do we need to keep the memory around until the library is freed?

		return result;
	}

	sampler device::create_sampler(
		filtering minification,
		filtering magnification,
		filtering mipmapping,
		float mip_lod_bias,
		float min_lod,
		float max_lod,
		std::optional<float> max_anisotropy,
		sampler_address_mode addressing_u,
		sampler_address_mode addressing_v,
		sampler_address_mode addressing_w,
		linear_rgba_f border_color,
		comparison_function comparison
	) {
		auto smp = NS::TransferPtr(MTL::SamplerDescriptor::alloc()->init());
		smp->setSAddressMode(_details::conversions::to_sampler_address_mode(addressing_u));
		smp->setTAddressMode(_details::conversions::to_sampler_address_mode(addressing_v));
		smp->setRAddressMode(_details::conversions::to_sampler_address_mode(addressing_w));
		// TODO border color?
		smp->setMinFilter(_details::conversions::to_sampler_min_mag_filter(minification));
		smp->setMagFilter(_details::conversions::to_sampler_min_mag_filter(magnification));
		smp->setMipFilter(_details::conversions::to_sampler_mip_filter(mipmapping));
		smp->setLodMinClamp(min_lod);
		smp->setLodMaxClamp(max_lod);
		smp->setMaxAnisotropy(static_cast<NS::UInteger>(max_anisotropy.value_or(1.0f)));
		smp->setCompareFunction(_details::conversions::to_compare_function(comparison));
		smp->setSupportArgumentBuffers(true);
		return sampler(NS::TransferPtr(_dev->newSamplerState(smp.get())), mip_lod_bias);
	}

	descriptor_set_layout device::create_descriptor_set_layout(
		std::span<const descriptor_range_binding> bindings, shader_stage stage
	) {
		descriptor_set_layout result = nullptr;
		result._bindings.insert(result._bindings.end(), bindings.begin(), bindings.end());
		result._stage = stage;
		std::sort(
			result._bindings.begin(),
			result._bindings.end(),
			[](descriptor_range_binding lhs, descriptor_range_binding rhs) {
				return lhs.register_index < rhs.register_index;
			}
		);
		// verify that there are no overlapping ranges
		for (std::size_t i = 1; i < result._bindings.size(); ++i) {
			crash_if(result._bindings[i].register_index <= result._bindings[i - 1].get_last_register_index());
		}
		return result;
	}

	pipeline_resources device::create_pipeline_resources(std::span<const gpu::descriptor_set_layout *const>) {
		// TODO
		return nullptr;
	}

	graphics_pipeline_state device::create_graphics_pipeline_state(
		const pipeline_resources &rsrc,
		const shader_binary *vs,
		const shader_binary *ps,
		const shader_binary *ds,
		const shader_binary *hs,
		const shader_binary *gs,
		std::span<const render_target_blend_options> blend,
		const rasterizer_options &rasterizer,
		const depth_stencil_options &depth_stencil,
		std::span<const input_buffer_layout> input_buffers,
		primitive_topology topology,
		const frame_buffer_layout &fb_layout,
		std::size_t num_viewports
	) {
		auto vert_descriptor = NS::TransferPtr(MTL::VertexDescriptor::alloc()->init());
		NS::UInteger num_elements = 0;
		for (const input_buffer_layout &input_layout : input_buffers) {
			const NS::UInteger buffer_index = kIRVertexBufferBindPoint + input_layout.buffer_index;
			MTL::VertexBufferLayoutDescriptor *buffer_layout = vert_descriptor->layouts()->object(buffer_index);
			buffer_layout->setStride(input_layout.stride);
			buffer_layout->setStepFunction(_details::conversions::to_vertex_step_function(input_layout.input_rate));
			for (const input_buffer_element &input_elem: input_layout.elements) {
				// find the index of this attribute
				std::string semantic = std::format(
					"{}{}", string::to_generic(input_elem.semantic_name), input_elem.semantic_index
				);
				for (char &c : semantic) {
					c = std::tolower(c);
				}
				NS::UInteger elem_index = std::numeric_limits<NS::UInteger>::max();
				for (std::size_t i = 0; i < vs->_vs_input_attributes.size(); ++i) {
					if (vs->_vs_input_attributes[i].name == string::assume_utf8(semantic)) {
						crash_if(elem_index != std::numeric_limits<NS::UInteger>::max()); // duplicate semantic
						elem_index = i;
					}
				}
				crash_if(elem_index == std::numeric_limits<NS::UInteger>::max());
				elem_index += kIRStageInAttributeStartIndex;

				// add vertex attribute
				MTL::VertexAttributeDescriptor *attr = vert_descriptor->attributes()->object(elem_index);
				attr->setFormat(_details::conversions::to_vertex_format(input_elem.element_format));
				attr->setOffset(input_elem.byte_offset);
				attr->setBufferIndex(buffer_index);
				++num_elements;
			}
		}

		auto descriptor = NS::TransferPtr(MTL::RenderPipelineDescriptor::alloc()->init());
		descriptor->setVertexFunction(vs->_get_single_function().get());
		descriptor->setFragmentFunction(ps->_get_single_function().get());
		// TODO tessellation shaders?
		descriptor->setVertexDescriptor(vert_descriptor.get());
		for (std::size_t i = 0; i < fb_layout.color_render_target_formats.size(); ++i) {
			const render_target_blend_options &blend_opts = blend[i];
			MTL::RenderPipelineColorAttachmentDescriptor *attachment = descriptor->colorAttachments()->object(i);
			attachment->setPixelFormat(_details::conversions::to_pixel_format(
				fb_layout.color_render_target_formats[i]
			));
			attachment->setWriteMask(_details::conversions::to_color_write_mask(blend_opts.write_mask));
			attachment->setBlendingEnabled(blend_opts.enabled);
			attachment->setAlphaBlendOperation(
				_details::conversions::to_blend_operation(blend_opts.alpha_operation)
			);
			attachment->setRgbBlendOperation(
				_details::conversions::to_blend_operation(blend_opts.color_operation)
			);
			attachment->setDestinationAlphaBlendFactor(
				_details::conversions::to_blend_factor(blend_opts.destination_alpha)
			);
			attachment->setDestinationRGBBlendFactor(
				_details::conversions::to_blend_factor(blend_opts.destination_color)
			);
			attachment->setSourceAlphaBlendFactor(
				_details::conversions::to_blend_factor(blend_opts.source_alpha)
			);
			attachment->setSourceRGBBlendFactor(
				_details::conversions::to_blend_factor(blend_opts.source_color)
			);
		}
		{
			const auto &fmt_props = format_properties::get(fb_layout.depth_stencil_render_target_format);
			const MTL::PixelFormat ds_format =
				_details::conversions::to_pixel_format(fb_layout.depth_stencil_render_target_format);
			if (fmt_props.has_depth()) {
				descriptor->setDepthAttachmentPixelFormat(ds_format);
			}
			if (fmt_props.has_stencil()) {
				descriptor->setStencilAttachmentPixelFormat(ds_format);
			}
		}
		descriptor->setInputPrimitiveTopology(_details::conversions::to_primitive_topology_class(topology));
		// TODO multisample and tessellation settings
		descriptor->setSupportIndirectCommandBuffers(true);
		descriptor->setShaderValidation(_details::conversions::to_shader_validation(_context_opts));

		auto ds_descriptor = NS::TransferPtr(MTL::DepthStencilDescriptor::alloc()->init());
		if (depth_stencil.enable_depth_testing) {
			ds_descriptor->setDepthCompareFunction(
				_details::conversions::to_compare_function(depth_stencil.depth_comparison)
			);
			ds_descriptor->setDepthWriteEnabled(depth_stencil.write_depth);
		}
		if (depth_stencil.enable_stencil_testing) {
			ds_descriptor->setBackFaceStencil(_details::conversions::to_stencil_descriptor(
				depth_stencil.stencil_back_face, depth_stencil.stencil_read_mask, depth_stencil.stencil_write_mask
			).get());
			ds_descriptor->setFrontFaceStencil(_details::conversions::to_stencil_descriptor(
				depth_stencil.stencil_front_face, depth_stencil.stencil_read_mask, depth_stencil.stencil_write_mask
			).get());
		}

		NS::Error *error = nullptr;
		auto pipeline_state = NS::TransferPtr(_dev->newRenderPipelineState(descriptor.get(), &error));
		if (error) {
			log().error("{}", error->localizedDescription()->utf8String());
			std::abort();
		}
		auto depth_stencil_state = NS::TransferPtr(_dev->newDepthStencilState(ds_descriptor.get()));
		// TODO pipeline resources?
		// TODO num viewports?
		return graphics_pipeline_state(
			std::move(pipeline_state), std::move(depth_stencil_state), rasterizer, topology
		);
	}

	compute_pipeline_state device::create_compute_pipeline_state(
		const pipeline_resources&, const shader_binary &shader
	) {
		NS::Error *error = nullptr;
		auto pipeline = NS::TransferPtr(_dev->newComputePipelineState(shader._get_single_function().get(), &error));
		if (error) {
			log().error("{}", error->localizedDescription()->utf8String());
			std::abort();
		}
		return compute_pipeline_state(std::move(pipeline), shader._thread_group_size);
	}

	std::span<const std::pair<memory_type_index, memory_properties>> device::enumerate_memory_types() const {
		constexpr static std::array _memory_types{
			std::pair(static_cast<memory_type_index>(_details::memory_type_index::shared_cpu_cached),   memory_properties::host_visible | memory_properties::host_cached),
			std::pair(static_cast<memory_type_index>(_details::memory_type_index::shared_cpu_uncached), memory_properties::host_visible                                 ),
			std::pair(static_cast<memory_type_index>(_details::memory_type_index::device_private),      memory_properties::device_local                                 ),
		};
		static_assert(
			std::size(_memory_types) == std::to_underlying(_details::memory_type_index::num_enumerators),
			"Missing memory types"
		);
		return _memory_types;
	}

	memory_block device::allocate_memory(std::size_t size, memory_type_index type) {
		auto heap_descriptor = NS::TransferPtr(MTL::HeapDescriptor::alloc()->init());
		heap_descriptor->setType(MTL::HeapTypePlacement);
		heap_descriptor->setResourceOptions(_details::conversions::to_resource_options(type));
		// TODO hazard tracking mode?
		heap_descriptor->setSize(size);
		return memory_block(NS::TransferPtr(_dev->newHeap(heap_descriptor.get())));
	}

	buffer device::create_committed_buffer(std::size_t size, memory_type_index type, buffer_usage_mask usages) {
		return buffer(NS::TransferPtr(
			_dev->newBuffer(size, _details::conversions::to_resource_options(type))
		));
	}

	image2d device::create_committed_image2d(
		cvec2u32 size,
		std::uint32_t mip_levels,
		format fmt,
		image_tiling tiling,
		image_usage_mask usages
	) {
		auto descriptor = _details::create_texture_descriptor(
			MTL::TextureType2DArray, // need to use array type for Metal-DXIR interop
			fmt,
			cvec3u32(size, 1),
			mip_levels,
			MTL::ResourceOptionCPUCacheModeWriteCombined | MTL::ResourceStorageModePrivate,
			usages
		);
		return image2d(NS::TransferPtr(_dev->newTexture(descriptor.get())));
	}

	image3d device::create_committed_image3d(
		cvec3u32 size,
		std::uint32_t mip_levels,
		format fmt,
		image_tiling tiling,
		image_usage_mask usages
	) {
		auto descriptor = _details::create_texture_descriptor(
			MTL::TextureType3D,
			fmt,
			size,
			mip_levels,
			MTL::ResourceOptionCPUCacheModeWriteCombined | MTL::ResourceStorageModePrivate,
			usages
		);
		return image3d(NS::TransferPtr(_dev->newTexture(descriptor.get())));
	}

	std::tuple<buffer, staging_buffer_metadata, std::size_t> device::create_committed_staging_buffer(
		cvec2u32 size, format fmt, memory_type_index mem_type, buffer_usage_mask usages
	) {
		// the buffer is tightly packed
		const auto &format_props = format_properties::get(fmt);
		const std::size_t bytes_per_row = size[0] * format_props.bytes_per_fragment;
		const std::size_t buf_size = bytes_per_row * size[1];
		buffer buf = create_committed_buffer(buf_size, mem_type, usages);
		staging_buffer_metadata result = uninitialized;
		result.image_size         = size;
		result.row_pitch_in_bytes = static_cast<std::uint32_t>(bytes_per_row);
		result.pixel_format       = fmt;
		return { std::move(buf), result, buf_size };
	}

	memory::size_alignment device::get_image2d_memory_requirements(
		cvec2u32 size,
		std::uint32_t mip_levels,
		format fmt,
		image_tiling tiling,
		image_usage_mask usages
	) {
		auto descriptor = _details::create_texture_descriptor(
			MTL::TextureType2DArray,
			fmt,
			cvec3u32(size, 1),
			mip_levels,
			0, // TODO what resource options should be used? does it affect anything?
			usages
		);
		return _details::conversions::back_to_size_alignment(_dev->heapTextureSizeAndAlign(descriptor.get()));
	}

	memory::size_alignment device::get_image3d_memory_requirements(
		cvec3u32 size,
		std::uint32_t mip_levels,
		format fmt,
		image_tiling tiling,
		image_usage_mask usages
	) {
		auto descriptor = _details::create_texture_descriptor(
			MTL::TextureType3D,
			fmt,
			size,
			mip_levels,
			0, // TODO what resource options should be used? does it affect anything?
			usages
		);
		return _details::conversions::back_to_size_alignment(_dev->heapTextureSizeAndAlign(descriptor.get()));
	}

	memory::size_alignment device::get_buffer_memory_requirements(std::size_t size, buffer_usage_mask usages) {
		// TODO what resource options should be used? does it affect anything?
		return _details::conversions::back_to_size_alignment(_dev->heapBufferSizeAndAlign(size, 0));
	}

	buffer device::create_placed_buffer(std::size_t size, buffer_usage_mask usages, const memory_block &mem, std::size_t offset) {
		return buffer(NS::TransferPtr(mem._heap->newBuffer(size, mem._heap->resourceOptions(), offset)));
	}

	image2d device::create_placed_image2d(
		cvec2u32 size,
		std::uint32_t mip_levels,
		format fmt,
		image_tiling tiling,
		image_usage_mask usages,
		const memory_block &mem,
		std::size_t offset
	) {
		auto descriptor = _details::create_texture_descriptor(
			MTL::TextureType2DArray,
			fmt,
			cvec3u32(size, 1),
			mip_levels,
			mem._heap->resourceOptions(),
			usages
		);
		return image2d(NS::TransferPtr(mem._heap->newTexture(descriptor.get(), offset)));
	}

	image3d device::create_placed_image3d(
		cvec3u32 size,
		std::uint32_t mip_levels,
		format fmt,
		image_tiling tiling,
		image_usage_mask usages,
		const memory_block &mem,
		std::size_t offset
	) {
		auto descriptor = _details::create_texture_descriptor(
			MTL::TextureType3D,
			fmt,
			size,
			mip_levels,
			mem._heap->resourceOptions(),
			usages
		);
		return image3d(NS::TransferPtr(mem._heap->newTexture(descriptor.get(), offset)));
	}

	std::byte *device::map_buffer(buffer &buf) {
		return static_cast<std::byte*>(buf._buf->contents());
	}

	void device::unmap_buffer(buffer&) {
	}

	void device::flush_mapped_buffer_to_host(buffer&, std::size_t, std::size_t) {
	}

	void device::flush_mapped_buffer_to_device(buffer&, std::size_t, std::size_t) {
	}

	image2d_view device::create_image2d_view_from(const image2d &img, format fmt, mip_levels mips) {
		if (img._tex->framebufferOnly()) {
			// cannot create views of framebuffer only textures
			// TODO check that the formats etc. match
			crash_if(_details::conversions::to_pixel_format(fmt) != img._tex->pixelFormat());
			return image2d_view(img._tex);
		}

		return image2d_view(NS::TransferPtr(img._tex->newTextureView(
			_details::conversions::to_pixel_format(fmt),
			MTL::TextureType2DArray,
			_details::conversions::to_range(mips, img._tex.get()),
			NS::Range(0, 1)
		)));
	}

	image3d_view device::create_image3d_view_from(const image3d &img, format fmt, mip_levels mips) {
		return image3d_view(NS::TransferPtr(img._tex->newTextureView(
			_details::conversions::to_pixel_format(fmt),
			MTL::TextureType3D,
			_details::conversions::to_range(mips, img._tex.get()),
			NS::Range(0, 1)
		)));
	}

	frame_buffer device::create_frame_buffer(
		std::span<const gpu::image2d_view *const> color_rts, const image2d_view *depth_stencil_rt, cvec2u32 size
	) {
		frame_buffer result = nullptr;
		result._color_rts.reserve(color_rts.size());
		for (const gpu::image2d_view *rt : color_rts) {
			result._color_rts.emplace_back(rt->_tex.get());
		}
		if (depth_stencil_rt) {
			result._depth_stencil_rt = depth_stencil_rt->_tex.get();
		}
		result._size = size;
		return result;
	}

	fence device::create_fence(synchronization_state) {
		// TODO
	}

	timeline_semaphore device::create_timeline_semaphore(gpu::_details::timeline_semaphore_value_type val) {
		auto event = NS::TransferPtr(_dev->newSharedEvent());
		event->setSignaledValue(val);
		return timeline_semaphore(std::move(event));
	}

	void device::reset_fence(fence&) {
		// TODO
	}

	void device::wait_for_fence(fence&) {
		// TODO
	}

	void device::signal_timeline_semaphore(
		timeline_semaphore &sem, gpu::_details::timeline_semaphore_value_type val
	) {
		sem._event->setSignaledValue(val);
	}

	gpu::_details::timeline_semaphore_value_type device::query_timeline_semaphore(timeline_semaphore &sem) {
		return sem._event->signaledValue();
	}

	void device::wait_for_timeline_semaphore(
		timeline_semaphore &sem, gpu::_details::timeline_semaphore_value_type val
	) {
		sem._event->waitUntilSignaledValue(val, std::numeric_limits<std::uint64_t>::max());
	}

	timestamp_query_heap device::create_timestamp_query_heap(std::uint32_t size) {
		// TODO
	}

	void device::fetch_query_results(
		timestamp_query_heap&, std::uint32_t first, std::span<std::uint64_t> timestamps
	) {
		// TODO
	}

	void device::set_debug_name(buffer &buf, const char8_t *name) {
		buf._buf->setLabel(_details::conversions::to_string(name).get());
	}

	void device::set_debug_name(image_base &img, const char8_t *name) {
		static_cast<_details::basic_image_base&>(img)._tex->setLabel(_details::conversions::to_string(name).get());
	}

	void device::set_debug_name(image_view_base &img, const char8_t *name) {
		static_cast<_details::basic_image_view_base&>(img)._tex->setLabel(
			_details::conversions::to_string(name).get()
		);
	}

	bottom_level_acceleration_structure_geometry device::create_bottom_level_acceleration_structure_geometry(
		std::span<const raytracing_geometry_view>
	) {
		// TODO
	}

	instance_description device::get_bottom_level_acceleration_structure_description(
		bottom_level_acceleration_structure&,
		mat44f trans,
		std::uint32_t id,
		std::uint8_t mask,
		std::uint32_t hit_group_offset,
		raytracing_instance_flags
	) const {
		// TODO
	}

	acceleration_structure_build_sizes device::get_bottom_level_acceleration_structure_build_sizes(
		const bottom_level_acceleration_structure_geometry&
	) {
		// TODO
	}

	acceleration_structure_build_sizes device::get_top_level_acceleration_structure_build_sizes(
		std::size_t instance_count
	) {
		// TODO
	}

	bottom_level_acceleration_structure device::create_bottom_level_acceleration_structure(
		buffer&, std::size_t offset, std::size_t size
	) {
		// TODO
	}

	top_level_acceleration_structure device::create_top_level_acceleration_structure(
		buffer&, std::size_t offset, std::size_t size
	) {
		// TODO
	}

	void device::write_descriptor_set_acceleration_structures(
		descriptor_set&,
		const descriptor_set_layout&,
		std::size_t first_register,
		std::span<gpu::top_level_acceleration_structure *const>
	) {
		// TODO
	}

	shader_group_handle device::get_shader_group_handle(const raytracing_pipeline_state&, std::size_t index) {
		// TODO
	}

	raytracing_pipeline_state device::create_raytracing_pipeline_state(
		std::span<const shader_function> hit_group_shaders,
		std::span<const hit_shader_group> hit_groups,
		std::span<const shader_function> general_shaders,
		std::size_t max_recursion_depth,
		std::size_t max_payload_size,
		std::size_t max_attribute_size,
		const pipeline_resources&
	) {
		// TODO
	}


	std::pair<device, std::vector<command_queue>> adapter::create_device(
		std::span<const queue_family> families
	) {
		std::vector<command_queue> queues;
		queues.reserve(families.size());
		for (queue_family fam : families) {
			auto ptr = NS::TransferPtr(_dev->newCommandQueue());
			queues.emplace_back(command_queue(std::move(ptr)));
		}
		return { device(_dev, _context_opts), std::move(queues) };
	}

	adapter_properties adapter::get_properties() const {
		adapter_properties result = uninitialized;
		result.name        = _details::conversions::back_to_string(_dev->name());
		result.is_software = false;
		result.is_discrete = _dev->location() != MTL::DeviceLocationBuiltIn;
		// TODO no way to query these?
		// https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf
		result.constant_buffer_alignment           = 32;
		result.acceleration_structure_alignment    = 1; // TODO
		result.shader_group_handle_size            = 1; // TODO
		result.shader_group_handle_alignment       = 1; // TODO
		result.shader_group_handle_table_alignment = 1; // TODO
		return result;
	}
}
