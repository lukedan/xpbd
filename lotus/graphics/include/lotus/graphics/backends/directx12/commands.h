#pragma once

/// \file
/// DirectX 12 command queues.

#include <span>

#include "lotus/color.h"
#include "lotus/math/aab.h"
#include "details.h"
#include "pass.h"
#include "resources.h"
#include "frame_buffer.h"
#include "pipeline.h"
#include "synchronization.h"

namespace lotus::graphics::backends::directx12 {
	class device;
	class command_queue;
	class command_allocator;


	/// A \p ID3D12CommandAllocator.
	class command_allocator {
		friend device;
		friend command_list;
	protected:
		/// Calls \p ID3D12CommandAllocator::Reset().
		void reset(device&);
	private:
		_details::com_ptr<ID3D12CommandAllocator> _allocator; ///< The allocator.
	};

	/// A \p ID3D12CommandList.
	class command_list {
		friend device;
		friend command_queue;
	protected:
		/// No initialization.
		command_list(std::nullptr_t) : _descriptor_heaps{ nullptr, nullptr } {
		}

		/// Calls \p ID3D12GraphicsCommandList::Reset().
		void reset_and_start(command_allocator&);

		/// Calls \p ID3D12GraphicsCommandList4::BeginRenderPass().
		void begin_pass(
			const pass_resources&, const frame_buffer&, std::span<const linear_rgba_f>, float, std::uint8_t
		);

		/// Calls \p ID3D12GraphicsCommandList::SetPipelineState().
		void bind_pipeline_state(const graphics_pipeline_state&);
		/// Calls \p ID3D12GraphicsCommandList::SetPipelineState().
		void bind_pipeline_state(const compute_pipeline_state&);
		/// Calls \p ID3D12GraphicsCommandList::IASetVertexBuffers().
		void bind_vertex_buffers(std::size_t, std::span<const vertex_buffer>);
		/// Calls \p ID3D12GraphicsCommandList::IASetIndexBuffer().
		void bind_index_buffer(const buffer&, std::size_t offset, index_format);
		/// Calls \p ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable() for all given descriptor sets.
		void bind_graphics_descriptor_sets(
			const pipeline_resources&, std::size_t first, std::span<const graphics::descriptor_set *const>
		);
		/// Calls \p ID3D12GraphicsCommandList::SetComputeRootDescriptorTable() for all given descriptor sets.
		void bind_compute_descriptor_sets(
			const pipeline_resources&, std::size_t first, std::span<const graphics::descriptor_set *const>
		);
		/// Calls \p ID3D12GraphicsCommandList::RSSetViewports().
		void set_viewports(std::span<const viewport>);
		/// Calls \p ID3D12GraphicsCommandList::RSSetScissorRects().
		void set_scissor_rectangles(std::span<const aab2i>);

		/// Calls \p ID3D12GraphicsCommandList::CopyBufferRegion().
		void copy_buffer(buffer &from, std::size_t off1, buffer &to, std::size_t off2, std::size_t size);
		/// Calls \p ID3D12GraphicsCommandList::CopyTextureRegion().
		void copy_image2d(
			image2d &from, subresource_index sub1, aab2s region, image2d &to, subresource_index sub2, cvec2s off
		);
		/// Calls \p ID3D12GraphicsCommandList::CopyTextureRegion().
		void copy_buffer_to_image(
			buffer &from, std::size_t byte_offset, staging_buffer_pitch, aab2s region,
			image2d &to, subresource_index subresource, cvec2s off
		);

		/// Calls \p ID3D12GraphicsCommandList::DrawInstanced().
		void draw_instanced(
			std::size_t first_vertex, std::size_t vertex_count,
			std::size_t first_instance, std::size_t instance_count
		);
		/// Calls \p ID3D12GraphicsCommandList::DrawIndexedInstanced().
		void draw_indexed_instanced(
			std::size_t first_index, std::size_t index_count,
			std::size_t first_vertex,
			std::size_t first_instance, std::size_t instance_count
		);
		/// Calls \p ID3D12GraphicsCommandList::Dispatch().
		void run_compute_shader(std::uint32_t x, std::uint32_t y, std::uint32_t z);

		/// Calls \p ID3D12GraphicsCommandList::ResourceBarrier() to insert a resource barrier.
		void resource_barrier(std::span<const image_barrier>, std::span<const buffer_barrier>);

		/// Calls \p ID3D12GraphicsCommandList4::EndRenderPass().
		void end_pass();
		/// Calls \p ID3D12GraphicsCommandList::Close().
		void finish();
	private:
		_details::com_ptr<ID3D12GraphicsCommandList4> _list; ///< The command list.
		std::array<ID3D12DescriptorHeap*, 2> _descriptor_heaps; ///< Descriptor heaps.
	};

	/// A DirectX 12 command queue.
	class command_queue {
		friend device;
		friend context;
	protected:
		/// Calls \p ID3D12CommandQueue::ExecuteCommandLists(), then optionally signals the fence using
		/// \p ID3D12CommandQueue::Signal().
		void submit_command_lists(std::span<const graphics::command_list *const>, fence*);
		/// Calls \p IDXGISwapChain::Present(), then signals any synchronization primitives associated with the
		/// current back buffer.
		void present(swap_chain&);
		/// Calls \p ID3D12CommandQueue::Signal().
		void signal(fence&);
	private:
		_details::com_ptr<ID3D12CommandQueue> _queue; ///< The command queue.
	};
}