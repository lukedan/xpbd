#pragma once

/// \file
/// DirectX 12 descriptor heaps.

#include <vector>

#include <d3d12.h>

namespace lotus::graphics::backends::directx12 {
	class command_list;
	class device;


	/// Since DirectX 12 disallows binding multiple descriptor heaps at the same time, this struct is simply for
	/// bookkeeping and the descriptor heaps are stored in the \ref device.
	class descriptor_pool {
		friend device;
	protected:
	private:
		// TODO actually do bookkeeping
	};

	/// An array of \p D3D12_DESCRIPTOR_RANGE1 objects.
	class descriptor_set_layout {
		friend device;
	protected:
	private:
		// TODO allocator
		std::vector<D3D12_DESCRIPTOR_RANGE1> _ranges; ///< Descriptor ranges.
		D3D12_SHADER_VISIBILITY _visibility; ///< Visibility to various shader stages.
		UINT _num_shader_resource_descriptors; ///< The number of shader resource descriptors.
		UINT _num_sampler_descriptors; ///< The number of sampler descriptors.
		/// The number of ranges in \ref _ranges that contain shader resources.
		std::size_t _num_shader_resource_ranges;
	};

	/// An array of descriptors.
	class descriptor_set {
		friend command_list;
		friend device;
	protected:
	private:
		_details::descriptor_range _shader_resource_descriptors = nullptr; ///< Shader resource descriptors.
		_details::descriptor_range _sampler_descriptors = nullptr; ///< Sampler descriptors.
	};
}
