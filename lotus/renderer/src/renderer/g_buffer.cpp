#include "lotus/renderer/g_buffer.h"

/// \file
/// Implementation of the G-buffer.

#include "lotus/renderer/context/asset_manager.h"
#include "lotus/renderer/shader_types.h"

namespace lotus::renderer::g_buffer {
	view view::create(context &ctx, cvec2u32 size, const pool &p) {
		view result = nullptr;
		result.albedo_glossiness = ctx.request_image2d(u8"GBuffer Albedo-glossiness", size, 1, albedo_glossiness_format, gpu::image_usage_mask::color_render_target | gpu::image_usage_mask::shader_read, p);
		result.normal            = ctx.request_image2d(u8"GBuffer Normal",            size, 1, normal_format,            gpu::image_usage_mask::color_render_target | gpu::image_usage_mask::shader_read, p);
		result.metalness         = ctx.request_image2d(u8"GBuffer Metalness",         size, 1, metalness_format,         gpu::image_usage_mask::color_render_target | gpu::image_usage_mask::shader_read, p);
		result.velocity          = ctx.request_image2d(u8"GBuffer Velocity",          size, 1, velocity_format,          gpu::image_usage_mask::color_render_target | gpu::image_usage_mask::shader_read, p);
		result.depth_stencil     = ctx.request_image2d(u8"GBuffer Depth-stencil",     size, 1, depth_stencil_format,     gpu::image_usage_mask::depth_stencil_render_target | gpu::image_usage_mask::shader_read, p);
		return result;
	}

	context::pass view::begin_pass(context &ctx) {
		return ctx.begin_pass(
			{
				image2d_color(albedo_glossiness, gpu::color_render_target_access::create_discard_then_write()),
				image2d_color(normal,            gpu::color_render_target_access::create_discard_then_write()),
				image2d_color(metalness,         gpu::color_render_target_access::create_discard_then_write()),
				image2d_color(velocity,          gpu::color_render_target_access::create_discard_then_write()),
			},
			image2d_depth_stencil(
				depth_stencil,
				gpu::depth_render_target_access::create_clear(0.0f),
				gpu::stencil_render_target_access::create_clear(0)
			),
			depth_stencil.get_size(),
			u8"G-Buffer pass"
		);
	}


	std::pair<assets::handle<assets::shader>, std::vector<input_buffer_binding>> pass_context::get_vertex_shader(
		context&, const assets::material::context_data &mat_ctx, const assets::geometry &geom
	) {
		std::vector<std::pair<std::u8string_view, std::u8string_view>> defines = {
			{ u8"LOTUS_MATERIAL_INCLUDE", mat_ctx.get_material_include() },
		};
		std::vector<input_buffer_binding> inputs;
		if (geom.vertex_buffer.data) {
			inputs.emplace_back(geom.vertex_buffer.into_input_buffer_binding(
				u8"POSITION", 0, static_cast<std::uint32_t>(inputs.size())
			));
		}
		if (geom.uv_buffer.data) {
			inputs.emplace_back(geom.uv_buffer.into_input_buffer_binding(
				u8"TEXCOORD", 0, static_cast<std::uint32_t>(inputs.size())
			));
			defines.emplace_back(u8"VERTEX_INPUT_HAS_UV", u8"");
		}
		if (geom.normal_buffer.data) {
			inputs.emplace_back(geom.normal_buffer.into_input_buffer_binding(
				u8"NORMAL", 0, static_cast<std::uint32_t>(inputs.size())
			));
			defines.emplace_back(u8"VERTEX_INPUT_HAS_NORMAL", u8"");
		}
		if (geom.tangent_buffer.data) {
			inputs.emplace_back(geom.tangent_buffer.into_input_buffer_binding(
				u8"TANGENT", 0, static_cast<std::uint32_t>(inputs.size())
			));
			defines.emplace_back(u8"VERTEX_INPUT_HAS_TANGENT", u8"");
		}
		auto shader = _man.compile_shader_in_filesystem(
			_man.asset_library_path / "shaders/standard_vertex_shader.hlsl",
			gpu::shader_stage::vertex_shader,
			u8"main_vs",
			defines
		);
		return { std::move(shader), std::move(inputs) };
	}

	assets::handle<assets::shader> pass_context::get_pixel_shader(
		context&, const assets::material::context_data &mat_ctx
	) {
		auto defines = mat_ctx.get_additional_ps_defines();
		defines.emplace_back(u8"LOTUS_MATERIAL_INCLUDE", mat_ctx.get_material_include());
		return _man.compile_shader_in_filesystem(
			_man.asset_library_path / "shaders/gbuffer_pixel_shader.hlsl",
			gpu::shader_stage::pixel_shader, u8"main_ps", defines
		);
	}


	void render_instances(
		context::pass &pass, assets::manager &man, std::span<const instance> instances,
		cvec2u32 viewport_size, mat44f view, mat44f projection, mat44f jitter, mat44f prev_projection_view
	) {
		pass_context pass_ctx(man);
		for (const auto &inst : instances) {
			if (!inst.material) {
				continue;
			}

			graphics_pipeline_state state(
				{
					gpu::render_target_blend_options::disabled(),
					gpu::render_target_blend_options::disabled(),
					gpu::render_target_blend_options::disabled(),
					gpu::render_target_blend_options::disabled(),
				},
				gpu::rasterizer_options(gpu::depth_bias_options::disabled(), gpu::front_facing_mode::counter_clockwise, gpu::cull_mode::cull_back, false),
				gpu::depth_stencil_options(true, true, gpu::comparison_function::greater, false, 0, 0, gpu::stencil_options::always_pass_no_op(), gpu::stencil_options::always_pass_no_op())
			);

			auto normal_trans_inv = mat::lup_decompose(inst.transform.block<3, 3>(0, 0));
			mat44f normal_trans = zero;
			normal_trans.set_block(
				0, 0, normal_trans_inv.invert().transposed() * std::pow(normal_trans_inv.determinant(), 2.0f / 3.0f)
			);
			shader_types::instance_data instance;
			instance.transform        = inst.transform;
			instance.normal_transform = normal_trans;
			instance.prev_transform   = inst.prev_transform;

			shader_types::view_data view_data;
			view_data.view                     = view;
			view_data.projection               = projection;
			view_data.jitter                   = jitter;
			view_data.projection_view          = projection * view;
			view_data.jittered_projection_view = jitter * projection * view;
			view_data.prev_projection_view     = prev_projection_view;
			view_data.rcp_viewport_size        = vec::memberwise_reciprocal(viewport_size.into<float>());

			auto additional_resources = all_resource_bindings(
				{
					{ 1, {
						{ 1, descriptor_resource::immediate_constant_buffer::create_for(instance) },
						{ 2, descriptor_resource::immediate_constant_buffer::create_for(view_data) },
					} },
				},
				{}
			);

			pass.draw_instanced(
				inst.geometry, inst.material, pass_ctx,
				{}, std::move(additional_resources), std::move(state), 1, u8"GBuffer instance"
			);
		}
	}
}
