#include "lotus/renderer/context/execution/queue_context.h"

/// \file
/// Implementation of command execution related functions.

#include "lotus/renderer/context/context.h"
#include "lotus/renderer/context/execution/batch_context.h"
#include "lotus/renderer/mipmap.h"

namespace lotus::renderer::execution {
	queue_context::queue_context(batch_context &batch_ctx, _details::queue_data &q) :
		early_statistics(zero),
		_batch_ctx(batch_ctx),
		_q(q) {

		_image_transitions.resize(q.batch_commands.size());
		_buffer_transitions.resize(q.batch_commands.size());
	}

	void queue_context::start_execution() {
		_next_timestamp = _timestamp_command_indices.begin();
		_next_acquire_event = _acquire_dependency_events.begin();
		_next_release_event = _release_dependency_events.begin();
		if (!_timestamp_command_indices.empty()) {
			_timestamps = &_batch_ctx.record_batch_resource(_get_device().create_timestamp_query_heap(
				static_cast<std::uint32_t>(_timestamp_command_indices.size())
			));
		}
	}

	void queue_context::execute_next_command() {
		const auto &cmd = _q.batch_commands[std::to_underlying(_command_index)];

		// check & insert timestamp
		if (_next_timestamp != _timestamp_command_indices.end() && *_next_timestamp == _command_index) {
			const auto timestamp_index = _next_timestamp - _timestamp_command_indices.begin();
			_get_command_list().query_timestamp(*_timestamps, static_cast<std::uint32_t>(timestamp_index));
			++_next_timestamp;
		}

		{ // acquire dependencies
			std::vector<gpu::timeline_semaphore_synchronization> waits;
			while (
				_next_acquire_event != _acquire_dependency_events.end() &&
				_next_acquire_event->command_index == _command_index
			) {
				auto &other_queue = _q.ctx._queues[_next_acquire_event->queue_index];
				waits.emplace_back(other_queue.semaphore, _next_acquire_event->data);
				++_next_acquire_event;
			}

			// first check for flushing the current command list - this is hit when these waits do not
			// immediately follow any notify events
			if (!waits.empty()) {
				_flush_command_list(nullptr, {});
				_pending_waits = std::move(waits);
			}
		}

		{ // handle transitions
			const auto &img_transitions = _image_transitions[std::to_underlying(_command_index)];
			const auto &buf_transitions = _buffer_transitions[std::to_underlying(_command_index)];
			if (!img_transitions.empty() && !buf_transitions.empty()) {
				_get_command_list().resource_barrier(img_transitions, buf_transitions);
			}
		}

		// execute command
		std::visit(
			[&](const auto &c) {
				_execute(c);
			},
			cmd.value
		);

		{ // release dependencies
			std::vector<gpu::timeline_semaphore_synchronization> signals;
			while (
				_next_release_event != _release_dependency_events.end() &&
				_next_release_event->command_index == _command_index
			) {
				signals.emplace_back(_q.semaphore, _next_release_event->data);
				++_next_release_event;
			}

			// second check for flushing the current command list - when there is a release event
			if (!signals.empty()) {
				_flush_command_list(nullptr, signals);
			}
		}

		_command_index = index::next(_command_index);
	}

	bool queue_context::is_finished() const {
		return std::to_underlying(_command_index) == _q.batch_commands.size();
	}

	void queue_context::finish_execution() {
		gpu::timeline_semaphore_synchronization last_signal(_q.semaphore, ++_q.semaphore_value);
		auto signals = { last_signal };
		_flush_command_list(nullptr, signals);

		auto &resolve_data = _get_queue_resolve_data();
		resolve_data.timestamp_heap = _timestamps;
		resolve_data.num_timestamps = static_cast<std::uint32_t>(_timestamp_command_indices.size());
	}

	gpu::command_list &queue_context::_get_command_list() {
		if (!_list) {
			if (!_cmd_alloc) {
				_cmd_alloc = &_batch_ctx.record_batch_resource(
					_get_device().create_command_allocator(_q.queue.get_type())
				);
			}
			_list = &_batch_ctx.record_batch_resource(_get_device().create_and_start_command_list(*_cmd_alloc));
		}
		return *_list;
	}

	void queue_context::_flush_command_list(
		gpu::fence *notify_fence, std::span<const gpu::timeline_semaphore_synchronization> notify_semaphores
	) {
		gpu::queue_synchronization sync(notify_fence, _pending_waits, notify_semaphores);
		if (_list) {
			_list->finish();
			_q.queue.submit_command_lists({ _list }, std::move(sync));
			_list = nullptr;
		} else {
			_q.queue.submit_command_lists({}, std::move(sync));
		}
		_pending_waits.clear();
	}

	void queue_context::_bind_descriptor_sets(
		const pipeline_resources_info &rsrc, descriptor_set_bind_point bind_point
	) {
		const auto &sets = rsrc.descriptor_sets;
		std::vector<const gpu::descriptor_set*> set_ptrs;
		for (const auto &s : sets) {
			set_ptrs.emplace_back(s.set);
		}

		for (std::size_t i = 0; i < sets.size(); ) {
			std::size_t end = i + 1;
			while (end < sets.size() && sets[end].space == sets[i].space + (end - i)) {
				++end;
			}

			switch (bind_point) {
			case descriptor_set_bind_point::graphics:
				_get_command_list().bind_graphics_descriptor_sets(
					*rsrc.pipeline_resources, sets[i].space, { set_ptrs.begin() + i, set_ptrs.begin() + end }
				);
				break;
			case descriptor_set_bind_point::compute:
				_get_command_list().bind_compute_descriptor_sets(
					*rsrc.pipeline_resources, sets[i].space, { set_ptrs.begin() + i, set_ptrs.begin() + end }
				);
				break;
			case descriptor_set_bind_point::raytracing:
				_get_command_list().bind_ray_tracing_descriptor_sets(
					*rsrc.pipeline_resources, sets[i].space, { set_ptrs.begin() + i, set_ptrs.begin() + end }
				);
			}

			i = end;
		}
	}

	void queue_context::_execute(const commands::copy_buffer &cmd) {
		_get_command_list().copy_buffer(
			cmd.source._ptr->data,
			cmd.source_offset,
			cmd.destination._ptr->data,
			cmd.destination_offset,
			cmd.size
		);
	}

	void queue_context::_execute(const commands::copy_buffer_to_image &cmd) {
		const auto dest = cmd.destination.highest_mip_with_warning();
		_get_command_list().copy_buffer_to_image(
			cmd.source._ptr->data,
			cmd.source_offset,
			cmd.staging_buffer_meta,
			dest._ptr->image,
			gpu::subresource_index::create_color(dest._mip_levels.first_level, 0),
			cmd.destination_offset
		);
	}

	void queue_context::_execute(const commands::build_blas &cmd) {
		// create geometry
		std::vector<gpu::raytracing_geometry_view> geometries;
		for (const auto &input : cmd.geometry) {
			geometries.emplace_back(
				batch_context::get_vertex_buffer_view(input),
				batch_context::get_index_buffer_view(input),
				input.flags
			);
		}
		auto geom = _get_device().create_bottom_level_acceleration_structure_geometry(geometries);
		const auto &build_sizes = _get_device().get_bottom_level_acceleration_structure_build_sizes(geom);
		// create scratch buffer
		// TODO allocate this from a pool?
		auto &scratch_buffer = _batch_ctx.record_batch_resource(_get_device().create_committed_buffer(
			build_sizes.build_scratch_size,
			_q.ctx.get_device_memory_type_index(),
			gpu::buffer_usage_mask::shader_read | gpu::buffer_usage_mask::shader_write
		));
		_get_command_list().build_acceleration_structure(geom, cmd.target._ptr->handle, scratch_buffer, 0);
	}

	void queue_context::_execute(const commands::build_tlas &cmd) {
		const auto build_sizes =
			_get_device().get_top_level_acceleration_structure_build_sizes(cmd.instances.size());
		// create input and scratch buffers
		// TODO allocate these from a pool?
		const std::size_t input_buffer_size = cmd.instances.size() * sizeof(gpu::instance_description);
		auto &input_buffer = _batch_ctx.record_batch_resource(_get_device().create_committed_buffer(
			input_buffer_size,
			_q.ctx.get_upload_memory_type_index(),
			gpu::buffer_usage_mask::shader_read
		));
		auto &scratch_buffer = _batch_ctx.record_batch_resource(_get_device().create_committed_buffer(
			build_sizes.build_scratch_size,
			_q.ctx.get_device_memory_type_index(),
			gpu::buffer_usage_mask::shader_read | gpu::buffer_usage_mask::shader_write
		));
		{ // copy data to input buffer
			auto *input_data = reinterpret_cast<gpu::instance_description*>(_get_device().map_buffer(input_buffer));
			for (std::size_t i = 0; i < cmd.instances.size(); ++i) {
				const auto &instance = cmd.instances[i];
				input_data[i] = _get_device().get_bottom_level_acceleration_structure_description(
					instance.acceleration_structure._ptr->handle,
					instance.transform,
					instance.id,
					instance.mask,
					instance.hit_group_offset,
					instance.flags
				);
			}
			_get_device().flush_mapped_buffer_to_device(input_buffer, 0, input_buffer_size);
			_get_device().unmap_buffer(input_buffer);
		}
		_get_command_list().build_acceleration_structure(
			input_buffer, 0, cmd.instances.size(), cmd.target._ptr->handle, scratch_buffer, 0
		);
	}

	void queue_context::_execute(const commands::begin_pass &cmd) {
		crash_if(_within_pass);
		// dirty pass-specific execution state
		crash_if(!_color_rt_formats.empty() || _depth_stencil_rt_format != gpu::format::none);
		_within_pass = true;

		// create frame buffer
		gpu::frame_buffer *frame_buffer = nullptr;
		std::vector<gpu::color_render_target_access> color_rt_access;
		{
			// color render targets
			std::vector<gpu::image2d_view*> color_rts;
			for (const auto &color_rt : cmd.color_render_targets) {
				if (std::holds_alternative<recorded_resources::image2d_view>(color_rt.view)) {
					auto rt = std::get<recorded_resources::image2d_view>(color_rt.view).highest_mip_with_warning();
					auto rt_size = mipmap::get_size(rt._ptr->size, rt._mip_levels.first_level);
					crash_if(rt_size != cmd.render_target_size);
					color_rts.emplace_back(&_q.ctx._request_image_view(rt));
					_color_rt_formats.emplace_back(rt._view_format);
				} else if (std::holds_alternative<recorded_resources::swap_chain>(color_rt.view)) {
					auto rt = std::get<recorded_resources::swap_chain>(color_rt.view);
					crash_if(rt._ptr->current_size != cmd.render_target_size);
					color_rts.emplace_back(&_q.ctx._request_swap_chain_view(rt));
					_color_rt_formats.emplace_back(rt._ptr->current_format);
				} else {
					std::abort(); // unhandled
				}
				color_rt_access.emplace_back(color_rt.access);
			}
			crash_if(_color_rt_formats.size() != color_rt_access.size());

			// depth render target
			const gpu::image2d_view *depth_view = nullptr;
			if (cmd.depth_stencil_target.view) {
				auto depth_rt = cmd.depth_stencil_target.view.highest_mip_with_warning();
				auto rt_size = mipmap::get_size(depth_rt._ptr->size, depth_rt._mip_levels.first_level);
				crash_if(rt_size != cmd.render_target_size);
				depth_view = &_q.ctx._request_image_view(depth_rt);
				_depth_stencil_rt_format = depth_rt._view_format;
			}

			frame_buffer = &_batch_ctx.record_batch_resource(
				_get_device().create_frame_buffer(color_rts, depth_view, cmd.render_target_size)
			);
		}

		_get_command_list().begin_pass(
			*frame_buffer,
			gpu::frame_buffer_access(
				color_rt_access, cmd.depth_stencil_target.depth_access, cmd.depth_stencil_target.stencil_access
			)
		);
	}

	void queue_context::_execute(const commands::draw_instanced &cmd) {
		crash_if(!_within_pass);

		pipeline_resources_info resources = _batch_ctx.use_pipeline_resources(cmd.resource_bindings);

		// build pipeline key and retrieve cached pipeline
		cache_keys::graphics_pipeline pipeline_key = nullptr;
		pipeline_key.pipeline_rsrc = resources.pipeline_resources_key;
		for (const auto &input_buffer : cmd.inputs) {
			pipeline_key.input_buffers.emplace_back(
				input_buffer.elements, input_buffer.stride, input_buffer.buffer_index, input_buffer.input_rate
			);
		}
		pipeline_key.color_rt_formats        = { _color_rt_formats.begin(), _color_rt_formats.end() };
		pipeline_key.dpeth_stencil_rt_format = _depth_stencil_rt_format;
		pipeline_key.vertex_shader           = cmd.vertex_shader;
		pipeline_key.pixel_shader            = cmd.pixel_shader;
		pipeline_key.pipeline_state          = cmd.state;
		pipeline_key.topology                = cmd.topology;
		const gpu::graphics_pipeline_state &pipeline = _q.ctx._cache.get_graphics_pipeline_state(pipeline_key);

		// gather vertex buffers
		std::vector<gpu::vertex_buffer> vertex_bufs;
		for (const auto &input : cmd.inputs) {
			if (input.buffer_index >= vertex_bufs.size()) {
				vertex_bufs.resize(input.buffer_index + 1, nullptr);
			}
			vertex_bufs[input.buffer_index] = gpu::vertex_buffer(input.data._ptr->data, input.offset, input.stride);
		}

		_get_command_list().bind_pipeline_state(pipeline);
		_bind_descriptor_sets(resources, descriptor_set_bind_point::graphics);
		_get_command_list().bind_vertex_buffers(0, vertex_bufs);
		if (cmd.index_buffer.data) {
			_get_command_list().bind_index_buffer(
				cmd.index_buffer.data._ptr->data, cmd.index_buffer.offset, cmd.index_buffer.format
			);
			_get_command_list().draw_indexed_instanced(0, cmd.index_count, 0, 0, cmd.instance_count);
		} else {
			_get_command_list().draw_instanced(0, cmd.vertex_count, 0, cmd.instance_count);
		}
	}

	void queue_context::_execute(const commands::end_pass &cmd) {
		crash_if(!_within_pass);
		_within_pass = false;
		// clean up pass-specific execution state
		_color_rt_formats.clear();
		_depth_stencil_rt_format = gpu::format::none;

		_get_command_list().end_pass();
	}

	void queue_context::_execute(const commands::dispatch_compute &cmd) {
		pipeline_resources_info resources = _batch_ctx.use_pipeline_resources(cmd.resources);
		// TODO: caching?
		auto &pipeline = _batch_ctx.record_batch_resource(
			_get_device().create_compute_pipeline_state(*resources.pipeline_resources, cmd.shader->binary)
		);
		auto &cmd_list = _get_command_list();
		cmd_list.bind_pipeline_state(pipeline);
		_bind_descriptor_sets(resources, descriptor_set_bind_point::compute);
		cmd_list.run_compute_shader(cmd.num_thread_groups[0], cmd.num_thread_groups[1], cmd.num_thread_groups[2]);
	}

	void queue_context::_execute(const commands::trace_rays &cmd) {
		// TODO
		std::abort();
	}

	void queue_context::_execute(const commands::present &cmd) {
		// if we haven't written anything to the swap chain, just don't present
		if (cmd.target._ptr->chain) {
			// flush pending commands
			_flush_command_list(nullptr, {});
			auto result = _q.queue.present(cmd.target._ptr->chain);
			if (result != gpu::swap_chain_status::ok) {
				log().warn("Presenting swap chain returned %d", static_cast<int>(result));
			}
		}
	}

	gpu::device &queue_context::_get_device() const {
		return _q.ctx._device;
	}

	batch_resolve_data::queue_data &queue_context::_get_queue_resolve_data() {
		return _batch_ctx.get_batch_resolve_data().queues[_q.queue.get_index()];
	}
}
