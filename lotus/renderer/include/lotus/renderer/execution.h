#pragma once

/// \file
/// Classes that are used during a \ref lotus::renderer::context's execution.

#include <cstddef>
#include <deque>
#include <vector>
#include <unordered_map>
#include <memory>

#include "lotus/gpu/resources.h"
#include "lotus/gpu/descriptors.h"
#include "lotus/gpu/pipeline.h"
#include "lotus/gpu/synchronization.h"
#include "lotus/gpu/commands.h"
#include "common.h"

namespace lotus::renderer {
	class context;
}

namespace lotus::renderer::execution {
	/// A batch of resources.
	struct batch_resources {
		std::deque<gpu::descriptor_set>            descriptor_sets;        ///< Descriptor sets.
		std::deque<gpu::descriptor_set_layout>     descriptor_set_layouts; ///< Descriptor set layouts.
		std::deque<gpu::pipeline_resources>        pipeline_resources;     ///< Pipeline resources.
		std::deque<gpu::compute_pipeline_state>    compute_pipelines;      ///< Compute pipeline states.
		std::deque<gpu::graphics_pipeline_state>   graphics_pipelines;     ///< Graphics pipeline states.
		std::deque<gpu::raytracing_pipeline_state> raytracing_pipelines;   ///< Raytracing pipeline states.
		std::deque<gpu::image2d>                   images;                 ///< Images.
		std::deque<gpu::image2d_view>              image_views;            ///< Image views.
		std::deque<gpu::buffer>                    buffers;                ///< Constant buffers.
		std::deque<gpu::command_list>              command_lists;          ///< Command lists.
		std::deque<gpu::frame_buffer>              frame_buffers;          ///< Frame buffers.
		std::deque<gpu::swap_chain>                swap_chains;            ///< Swap chains.
		std::deque<gpu::fence>                     fences;                 ///< Fences.

		std::vector<std::unique_ptr<_details::image2d>>    image2d_meta;    ///< Images to be disposed next frame.
		std::vector<std::unique_ptr<_details::swap_chain>> swap_chain_meta; ///< Swap chain to be disposed next frame.
		std::vector<std::unique_ptr<_details::buffer>>     buffer_meta;     ///< Buffers to be disposed next frame.

		/// Registers the given object as a resource.
		template <typename T> T &record(T &&obj) {
			if constexpr (std::is_same_v<T, gpu::descriptor_set>) {
				return descriptor_sets.emplace_back(std::move(obj));
			} else if constexpr (std::is_same_v<T, gpu::descriptor_set_layout>) {
				return descriptor_set_layouts.emplace_back(std::move(obj));
			} else if constexpr (std::is_same_v<T, gpu::pipeline_resources>) {
				return pipeline_resources.emplace_back(std::move(obj));
			} else if constexpr (std::is_same_v<T, gpu::compute_pipeline_state>) {
				return compute_pipelines.emplace_back(std::move(obj));
			} else if constexpr (std::is_same_v<T, gpu::graphics_pipeline_state>) {
				return graphics_pipelines.emplace_back(std::move(obj));
			} else if constexpr (std::is_same_v<T, gpu::raytracing_pipeline_state>) {
				return raytracing_pipelines.emplace_back(std::move(obj));
			} else if constexpr (std::is_same_v<T, gpu::image2d>) {
				return images.emplace_back(std::move(obj));
			} else if constexpr (std::is_same_v<T, gpu::image2d_view>) {
				return image_views.emplace_back(std::move(obj));
			} else if constexpr (std::is_same_v<T, gpu::buffer>) {
				return buffers.emplace_back(std::move(obj));
			} else if constexpr (std::is_same_v<T, gpu::command_list>) {
				return command_lists.emplace_back(std::move(obj));
			} else if constexpr (std::is_same_v<T, gpu::swap_chain>) {
				return swap_chains.emplace_back(std::move(obj));
			} else if constexpr (std::is_same_v<T, gpu::fence>) {
				return fences.emplace_back(std::move(obj));
			} else {
				static_assert(sizeof(T*) == 0, "Unhandled resource type");
			}
		}
	};

	/// Structures recording resource transition operations.
	namespace transition_records {
		/// Contains information about a layout transition operation.
		struct image2d {
			/// Initializes this structure to empty.
			image2d(std::nullptr_t) : mip_levels(gpu::mip_levels::all()) {
			}
			/// Initializes all fields of this struct.
			image2d(_details::image2d &img, gpu::mip_levels mips, _details::image_access acc) :
				target(&img), mip_levels(mips), access(acc) {
			}

			_details::image2d *target = nullptr; ///< The surface to transition.
			gpu::mip_levels mip_levels; ///< Mip levels to transition.
			_details::image_access access = uninitialized; ///< Access to transition to.
		};
		/// Contains information about a buffer transition operation.
		struct buffer {
			/// Initializes this structure to empty.
			buffer(std::nullptr_t) {
			}
			/// Initializes all fields of this struct.
			buffer(_details::buffer &buf, _details::buffer_access acc) : target(&buf), access(acc) {
			}

			/// Default equality and inequality comparisons.
			[[nodiscard]] friend bool operator==(const buffer&, const buffer&) = default;

			_details::buffer *target = nullptr; ///< The buffer to transition.
			_details::buffer_access access = uninitialized; ///< Access to transition to.
		};
		// TODO convert this into "generic image transition"
		/// Contains information about a layout transition operation.
		struct swap_chain {
			/// Initializes this structure to empty.
			swap_chain(std::nullptr_t) {
			}
			/// Initializes all fields of this struct.
			swap_chain(_details::swap_chain &c, _details::image_access acc) :
				target(&c), access(acc) {
			}

			_details::swap_chain *target = nullptr; ///< The swap chain to transition.
			_details::image_access access = uninitialized; ///< Access to transition to.
		};
	}

	/// A buffer for all resource transition operations.
	class transition_buffer {
	public:
		/// Initializes this buffer to empty.
		transition_buffer(std::nullptr_t) {
		}

		/// Stages a image transition operation, and notifies any descriptor arrays affected.
		void stage_transition(_details::image2d&, gpu::mip_levels, _details::image_access);
		/// Stages a buffer transition operation.
		void stage_transition(_details::buffer&, _details::buffer_access);
		/// Stages a swap chain transition operation.
		void stage_transition(_details::swap_chain &chain, _details::image_access usage) {
			_swap_chain_transitions.emplace_back(chain, usage);
		}
		/// Stages a raw buffer transition operation. No state tracking is performed for such operations; this is
		/// only intended to be used internally when the usage of a buffer is known.
		void stage_transition(gpu::buffer &buf, _details::buffer_access from, _details::buffer_access to) {
			std::pair<_details::buffer_access, _details::buffer_access> usage(from, to);
			auto [it, inserted] = _raw_buffer_transitions.emplace(&buf, usage);
			assert(inserted || it->second == usage);
		}
		/// Stages all pending transitions from the given image descriptor array.
		void stage_all_transitions_for(_details::image_descriptor_array&);
		/// Stages all pending transitions from the given buffer descriptor array.
		void stage_all_transitions_for(_details::buffer_descriptor_array&);

		/// Prepares this buffer for execution.
		void prepare();
		/// Collects all staged transition operations. \ref prepare() must have been called after all transitions
		/// have been staged.
		[[nodiscard]] std::pair<
			std::vector<gpu::image_barrier>, std::vector<gpu::buffer_barrier>
		> collect_transitions() const;
	private:
		/// Staged image transition operations.
		std::vector<transition_records::image2d> _image2d_transitions;
		/// Staged buffer transition operations.
		std::vector<transition_records::buffer> _buffer_transitions;
		/// Staged swap chain transition operations.
		std::vector<transition_records::swap_chain> _swap_chain_transitions;
		/// Staged raw buffer transition operations.
		std::unordered_map<
			gpu::buffer*, std::pair<_details::buffer_access, _details::buffer_access>
		> _raw_buffer_transitions;
	};

	/// Manages the execution of a series of commands.
	class context {
	public:
		/// 1MB for immediate constant buffers.
		constexpr static std::size_t immediate_constant_buffer_cache_size = 1 * 1024 * 1024;

		/// Creates a new execution context for the given context.
		[[nodiscard]] static context create(renderer::context&);
		/// No move construction.
		context(const context&) = delete;
		/// No move assignment.
		context &operator=(const context&) = delete;

		/// Creates the command list if necessary, and returns the current command list.
		[[nodiscard]] gpu::command_list &get_command_list();
		/// Submits the current command list.
		///
		/// \return Whether a command list has been submitted. If not, an empty submission will have been
		///         performed with the given synchronization requirements.
		bool submit(gpu::command_queue &q, gpu::queue_synchronization sync) {
			flush_immediate_constant_buffers();

			if (!_list) {
				q.submit_command_lists({}, std::move(sync));
				return false;
			}

			_list->finish();
			q.submit_command_lists({ _list }, std::move(sync));
			_list = nullptr;
			return true;
		}

		/// Records the given object to be disposed of when this frame finishes.
		template <typename T> T &record(T &&obj) {
			return _resources.record(std::move(obj));
		}
		/// Creates a new buffer with the given parameters.
		[[nodiscard]] gpu::buffer &create_buffer(std::size_t size, gpu::memory_type_index, gpu::buffer_usage_mask);
		/// Creates a frame buffer with the given parameters.
		[[nodiscard]] gpu::frame_buffer &create_frame_buffer(
			std::span<const gpu::image2d_view *const> color_rts,
			const gpu::image2d_view *ds_rt,
			cvec2s size
		);

		/// Allocates space for an immediate constant buffer.
		///
		/// \return A reference to the allocated region, and a pointer to the buffer data. The caller should
		///         immediately copy over the buffer's data.
		[[nodiscard]] std::pair<
			gpu::constant_buffer_view, void*
		> stage_immediate_constant_buffer(memory::size_alignment);
		/// Allocates an immediate constant buffer and copies the data over.
		///
		/// \param data Buffer data.
		/// \param alignment Alignment of the buffer, or zero to use the default alignment.
		[[nodiscard]] gpu::constant_buffer_view stage_immediate_constant_buffer(
			std::span<const std::byte> data, std::size_t alignment = 0
		);
		/// Flushes all staged immediate constant buffers.
		void flush_immediate_constant_buffers();

		/// Flushes all writes to the given image descriptor array, waiting if necessary.
		void flush_descriptor_array_writes(
			_details::image_descriptor_array&, const gpu::descriptor_set_layout&
		);
		/// Flushes all writes to the given buffer descriptor array, waiting if necessary.
		void flush_descriptor_array_writes(
			_details::buffer_descriptor_array&, const gpu::descriptor_set_layout&
		);

		/// Flushes all staged transitions.
		void flush_transitions();

		transition_buffer transitions; ///< Transitions.
	private:
		/// Initializes this context.
		context(renderer::context &ctx, batch_resources &rsrc) :
			transitions(nullptr),
			_ctx(ctx), _resources(rsrc),
			_immediate_constant_device_buffer(nullptr),
			_immediate_constant_upload_buffer(nullptr) {
		}

		renderer::context &_ctx; ///< The associated context.
		batch_resources &_resources; ///< Where to record internal resources created during execution to.
		gpu::command_list *_list = nullptr; ///< Current command list.

		/// Amount used in \ref _immediate_constant_device_buffer.
		std::size_t _immediate_constant_buffer_used = 0;
		/// Buffer containing all immediate constant buffers, located on the device memory.
		gpu::buffer _immediate_constant_device_buffer;
		/// Upload buffer for \ref _immediate_constant_device_buffer.
		gpu::buffer _immediate_constant_upload_buffer;
		/// Mapped pointer for \ref _immediate_constant_upload_buffer.
		std::byte *_immediate_constant_upload_buffer_ptr = nullptr;
	};
}