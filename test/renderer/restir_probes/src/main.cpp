#include <random>

#include <scene.h>
#include <camera_control.h>

#include <lotus/utils/camera.h>
#include <lotus/renderer/g_buffer.h>
#include <lotus/renderer/debug_drawing.h>

#include <lotus/renderer/shader_types_include_wrapper.h>
namespace shader_types {
#include "shaders/shader_types.hlsli"
}
#include <lotus/renderer/shader_types_include_wrapper.h>

#include <imgui.h>
#include <lotus/renderer/dear_imgui.h>
#include <lotus/system/dear_imgui.h>

template <typename T> struct ImGuiAutoDataType;
template <> struct ImGuiAutoDataType<std::uint32_t> {
	constexpr static ImGuiDataType value = ImGuiDataType_U32;
};
template <typename T> constexpr static ImGuiDataType ImGuiAutoDataType_v = ImGuiAutoDataType<T>::value;

template <typename T> static bool ImGui_SliderT(const char *label, T *data, T min, T max, const char *format = nullptr, ImGuiSliderFlags flags = 0) {
	return ImGui::SliderScalar(label, ImGuiAutoDataType_v<T>, data, &min, &max, format, flags);
}

int main(int argc, char **argv) {
	lsys::application app(u8"ReSTIR Probes");
	auto wnd = app.create_window();

	//auto options = lgpu::context_options::enable_validation | lgpu::context_options::enable_debug_info;
	auto options = lgpu::context_options::none;
	auto gctx = lgpu::context::create(options);
	lgpu::device gdev = nullptr;
	lgpu::adapter_properties gprop = uninitialized;
	gctx.enumerate_adapters([&](lgpu::adapter adap) -> bool {
		gprop = adap.get_properties();
		log().debug<u8"Device: {}">(lstr::to_generic(gprop.name));
		if (gprop.is_discrete) {
			log().debug<u8"Selected">();
			gdev = adap.create_device();
			return false;
		}
		return true;
	});
	auto gcmdq = gdev.create_command_queue();
	auto gshu = lgpu::shader_utility::create();

	auto rctx = lren::context::create(gctx, gprop, gdev, gcmdq);
	auto rassets = lren::assets::manager::create(rctx, &gshu);
	rassets.shader_library_path = "D:/Documents/Projects/lotus/lotus/renderer/include/lotus/renderer/shaders";
	rassets.additional_shader_includes = {
		"D:/Documents/Projects/lotus/lotus/renderer/include/lotus/renderer/shaders",
		"D:/Documents/Projects/lotus/test/renderer/common/include",
	};

	scene_representation scene(rassets);
	for (int i = 1; i < argc; ++i) {
		scene.load(argv[i]);
	}
	scene.finish_loading();

	auto debug_render = lren::debug_renderer::create(rassets);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::StyleColorsDark();
	auto imgui_rctx = lren::dear_imgui::context::create(rassets);
	auto imgui_sctx = lsys::dear_imgui::context::create();

	cvec2s window_size = zero;
	std::uint32_t frame_index = 0;

	auto swapchain = rctx.request_swap_chain(u8"Main Swap Chain", wnd, 2, { lgpu::format::r8g8b8a8_srgb, lgpu::format::b8g8r8a8_srgb });

	auto fill_buffer_cs = rassets.compile_shader_in_filesystem("src/shaders/fill_buffer.hlsl", lgpu::shader_stage::compute_shader, u8"main_cs");
	auto fs_quad_vs = rassets.compile_shader_in_filesystem(rassets.shader_library_path / "utils/fullscreen_quad_vs.hlsl", lgpu::shader_stage::vertex_shader, u8"main_vs");
	auto blit_ps = rassets.compile_shader_in_filesystem(rassets.shader_library_path / "utils/blit_ps.hlsl", lgpu::shader_stage::pixel_shader, u8"main_ps");
	auto show_gbuffer_ps = rassets.compile_shader_in_filesystem("src/shaders/gbuffer_visualization.hlsl", lgpu::shader_stage::pixel_shader, u8"main_ps");
	auto visualize_probes_vs = rassets.compile_shader_in_filesystem("src/shaders/visualize_probes.hlsl", lgpu::shader_stage::vertex_shader, u8"main_vs");
	auto visualize_probes_ps = rassets.compile_shader_in_filesystem("src/shaders/visualize_probes.hlsl", lgpu::shader_stage::pixel_shader, u8"main_ps");
	auto shade_point_debug_cs = rassets.compile_shader_in_filesystem("src/shaders/shade_point_debug.hlsl", lgpu::shader_stage::compute_shader, u8"main_cs");
	auto lighting_combine_ps = rassets.compile_shader_in_filesystem("src/shaders/combine_lighting_ps.hlsl", lgpu::shader_stage::pixel_shader, u8"main_ps");

	auto direct_update_cs = rassets.compile_shader_in_filesystem("src/shaders/direct_reservoirs.hlsl", lgpu::shader_stage::compute_shader, u8"main_cs");
	auto indirect_update_cs = rassets.compile_shader_in_filesystem("src/shaders/indirect_reservoirs.hlsl", lgpu::shader_stage::compute_shader, u8"main_cs");
	auto summarize_probes_cs = rassets.compile_shader_in_filesystem("src/shaders/summarize_probes.hlsl", lgpu::shader_stage::compute_shader, u8"main_cs");
	auto indirect_spatial_reuse_cs = rassets.compile_shader_in_filesystem("src/shaders/indirect_spatial_reuse.hlsl", lgpu::shader_stage::compute_shader, u8"main_cs");
	auto indirect_specular_cs = rassets.compile_shader_in_filesystem("src/shaders/indirect_specular.hlsl", lgpu::shader_stage::compute_shader, u8"main_cs");
	auto lighting_cs = rassets.compile_shader_in_filesystem("src/shaders/lighting.hlsl", lgpu::shader_stage::compute_shader, u8"main_cs");

	auto runtime_tex_pool = rctx.request_pool(u8"Run-time Textures");
	auto runtime_buf_pool = rctx.request_pool(u8"Run-time Buffers");

	auto cam_params = lotus::camera_parameters<float>::create_look_at(zero, cvec3f(100.0f, 100.0f, 100.0f));
	camera_control<float> cam_control(cam_params);

	float lighting_scale = 1.0f;
	int lighting_mode = 1;
	cvec3u32 probe_density(10, 10, 10);
	std::uint32_t direct_reservoirs_per_probe = 2;
	std::uint32_t indirect_reservoirs_per_probe = 4;
	std::uint32_t direct_sample_count_cap = 100;
	std::uint32_t indirect_sample_count_cap = 100;
	auto probe_bounds = lotus::aab3f::create_from_min_max({ -10.0f, -10.0f, -10.0f }, { 10.0f, 10.0f, 10.0f });
	float visualize_probe_size = 0.1f;
	int visualize_probes_mode = 0;
	int shade_point_debug_mode = 0;
	bool trace_shadow_rays_naive = true;
	bool trace_shadow_rays_reservoir = false;
	float diffuse_mul = 1.0f;
	float specular_mul = 1.0f;
	bool use_indirect_diffuse = true;
	bool use_indirect_specular = true;
	bool enable_indirect_specular_mis = true;
	bool update_probes = true;
	bool update_probes_this_frame = false;
	bool indirect_spatial_reuse = true;
	int indirect_spatial_reuse_visibility_test_mode = 0;
	int gbuffer_visualization = 0;

	std::uint32_t num_accumulated_frames = 0;

	shader_types::probe_constants probe_constants;

	lren::image2d_view path_tracer_accum = nullptr;

	lren::structured_buffer_view direct_reservoirs = nullptr;
	lren::structured_buffer_view indirect_reservoirs = nullptr;
	lren::structured_buffer_view probe_sh = nullptr;

	auto fill_buffer = [&](lren::structured_buffer_view buf, std::uint32_t value, std::u8string_view description) {
		buf = buf.view_as<std::uint32_t>();
		shader_types::fill_buffer_constants data;
		data.size = buf.get_num_elements();
		data.value = value;
		rctx.run_compute_shader_with_thread_dimensions(
			fill_buffer_cs, cvec3u32(data.size, 1, 1),
			lren::all_resource_bindings(
				{
					{ 0, {
						{ 0, buf.bind_as_read_write() },
						{ 1, lren_bds::immediate_constant_buffer::create_for(data) },
					} },
				},
				{}
			),
			description
		);
	};

	auto resize_probe_buffers = [&]() {
		uint32_t num_probes = probe_density[0] * probe_density[1] * probe_density[2];

		std::uint32_t num_direct_reservoirs = num_probes * direct_reservoirs_per_probe;
		direct_reservoirs = rctx.request_structured_buffer<shader_types::direct_lighting_reservoir>(
			u8"Direct Lighting Reservoirs", num_direct_reservoirs,
			lgpu::buffer_usage_mask::shader_read | lgpu::buffer_usage_mask::shader_write,
			runtime_buf_pool
		);
		std::uint32_t num_indirect_reservoirs = num_probes * indirect_reservoirs_per_probe;
		indirect_reservoirs = rctx.request_structured_buffer<shader_types::indirect_lighting_reservoir>(
			u8"Indirect Lighting Reservoirs", num_indirect_reservoirs,
			lgpu::buffer_usage_mask::shader_read | lgpu::buffer_usage_mask::shader_write,
			runtime_buf_pool
		);
		probe_sh = rctx.request_structured_buffer<shader_types::probe_data>(
			u8"Probe Data", num_probes,
			lgpu::buffer_usage_mask::shader_read | lgpu::buffer_usage_mask::shader_write,
			runtime_buf_pool
		);

		fill_buffer(direct_reservoirs, 0, u8"Clear Direct Reservoir Buffer");
		fill_buffer(indirect_reservoirs, 0, u8"Clear Indirect Reservoir Buffer");
		fill_buffer(probe_sh, 0, u8"Clear Probes Buffer");

		// compute transformation matrices
		cvec3f grid_size = probe_bounds.signed_size();
		mat33f rotscale = mat33f::diagonal(grid_size).inverse();
		mat44f world_to_grid = mat44f::identity();
		world_to_grid.set_block(0, 0, rotscale);
		world_to_grid.set_block(0, 3, rotscale * -probe_bounds.min);

		probe_constants.world_to_grid = world_to_grid;
		probe_constants.grid_to_world = world_to_grid.inverse();
		probe_constants.grid_size = probe_density;
		probe_constants.direct_reservoirs_per_probe = direct_reservoirs_per_probe;
		probe_constants.indirect_reservoirs_per_probe = indirect_reservoirs_per_probe;
	};

	resize_probe_buffers();

	auto on_resize = [&](lsys::window_events::resize &resize) {
		window_size = resize.new_size;

		swapchain.resize(window_size);
		cam_params.aspect_ratio = window_size[0] / static_cast<float>(window_size[1]);

		imgui_sctx.on_resize(resize);

		path_tracer_accum = rctx.request_image2d(u8"Path Tracer Accumulation Buffer", window_size, 1, lgpu::format::r32g32b32a32_float, lgpu::image_usage_mask::shader_read | lgpu::image_usage_mask::shader_write, runtime_tex_pool);
	};
	wnd.on_resize = [&](lsys::window_events::resize &resize) {
		on_resize(resize);
	};

	wnd.on_close_request = [&](lsys::window_events::close_request &req) {
		req.should_close = true;
		app.quit();
	};

	auto on_mouse_move = [&](lsys::window_events::mouse::move &move) {
		imgui_sctx.on_mouse_move(move);
		if (!ImGui::GetIO().WantCaptureMouse) {
			if (cam_control.on_mouse_move(move.new_position)) {
				num_accumulated_frames = 0;
			}
		}
	};
	wnd.on_mouse_move = [&](lsys::window_events::mouse::move &move) {
		on_mouse_move(move);
	};

	auto on_mouse_down = [&](lsys::window_events::mouse::button_down &down) {
		imgui_sctx.on_mouse_down(down);
		if (!ImGui::GetIO().WantCaptureMouse) {
			if (cam_control.on_mouse_down(down.button)) {
				wnd.acquire_mouse_capture();
			}
		}
	};
	wnd.on_mouse_button_down = [&](lsys::window_events::mouse::button_down &down) {
		on_mouse_down(down);
	};

	auto on_mouse_up = [&](lsys::window_events::mouse::button_up &up) {
		imgui_sctx.on_mouse_up(up);
		if (!ImGui::GetIO().WantCaptureMouse) {
			if (cam_control.on_mouse_up(up.button)) {
				wnd.release_mouse_capture();
			}
		}
	};
	wnd.on_mouse_button_up = [&](lsys::window_events::mouse::button_up &up) {
		on_mouse_up(up);
	};

	auto on_mouse_scroll = [&](lsys::window_events::mouse::scroll &sc) {
		imgui_sctx.on_mouse_scroll(sc);
	};
	wnd.on_mouse_scroll = [&](lsys::window_events::mouse::scroll &sc) {
		on_mouse_scroll(sc);
	};

	auto on_capture_broken = [&]() {
		cam_control.on_capture_broken();
	};
	wnd.on_capture_broken = [&]() {
		on_capture_broken();
	};


	wnd.show_and_activate();
	while (app.process_message_nonblocking() != lsys::message_type::quit) {
		if (window_size == zero) {
			continue;
		}

		rassets.update();

		{
			auto cam = cam_params.into_camera();

			auto g_buf = lren::g_buffer::view::create(rctx, window_size, runtime_tex_pool);
			{ // g-buffer
				auto pass = g_buf.begin_pass(rctx);
				lren::g_buffer::render_instances(pass, rassets, scene.instances, cam.view_matrix, cam.projection_matrix);
				pass.end();
			}

			auto light_diffuse = rctx.request_image2d(
				u8"Lighting Diffuse", window_size, 1, lgpu::format::r16g16b16a16_float,
				lgpu::image_usage_mask::shader_read | lgpu::image_usage_mask::shader_write,
				runtime_tex_pool
			);
			auto light_specular = rctx.request_image2d(
				u8"Lighting Specular", window_size, 1, lgpu::format::r16g16b16a16_float,
				lgpu::image_usage_mask::shader_read | lgpu::image_usage_mask::shader_write,
				runtime_tex_pool
			);

			shader_types::lighting_constants lighting_constants;
			lighting_constants.inverse_projection_view         = cam.inverse_projection_view_matrix;
			lighting_constants.camera                          = cvec4f(cam_params.position, 1.0f);
			lighting_constants.depth_linearization_constants   = cam.depth_linearization_constants;
			lighting_constants.screen_size                     = window_size.into<std::uint32_t>();
			lighting_constants.num_lights                      = scene.lights.size();
			lighting_constants.trace_shadow_rays_for_naive     = trace_shadow_rays_naive;
			lighting_constants.trace_shadow_rays_for_reservoir = trace_shadow_rays_reservoir;
			lighting_constants.lighting_mode                   = lighting_mode;
			lighting_constants.direct_diffuse_multiplier       = diffuse_mul;
			lighting_constants.direct_specular_multiplier      = specular_mul;
			lighting_constants.use_indirect                    = use_indirect_diffuse;

			if (update_probes || update_probes_this_frame) {
				update_probes_this_frame = false;

				{ // direct probes
					shader_types::direct_reservoir_update_constants direct_update_constants;
					direct_update_constants.num_lights       = scene.lights.size();
					direct_update_constants.sample_count_cap = direct_sample_count_cap;
					direct_update_constants.frame_index      = frame_index;

					lren::all_resource_bindings resources(
						{},
						{
							{ u8"probe_consts",      lren_bds::immediate_constant_buffer::create_for(probe_constants) },
							{ u8"constants",         lren_bds::immediate_constant_buffer::create_for(direct_update_constants) },
							{ u8"direct_reservoirs", direct_reservoirs.bind_as_read_write() },
							{ u8"all_lights",        scene.lights_buffer.bind_as_read_only() },
							{ u8"rtas",              scene.tlas },
						}
					);
					rctx.run_compute_shader_with_thread_dimensions(
						direct_update_cs, probe_density, std::move(resources), u8"Update Direct Probes"
					);
				}

				{ // indirect probes
					shader_types::indirect_reservoir_update_constants indirect_update_constants;
					indirect_update_constants.frame_index      = frame_index;
					indirect_update_constants.sample_count_cap = indirect_sample_count_cap;

					lren::all_resource_bindings resources(
						{
							{ 8, rassets.get_samplers() },
						},
						{
							{ u8"probe_consts",    lren_bds::immediate_constant_buffer::create_for(probe_constants) },
							{ u8"constants",       lren_bds::immediate_constant_buffer::create_for(indirect_update_constants) },
							{ u8"direct_probes",   direct_reservoirs.bind_as_read_only() },
							{ u8"indirect_sh",     probe_sh.bind_as_read_only() },
							{ u8"indirect_probes", indirect_reservoirs.bind_as_read_write() },
							{ u8"rtas",            scene.tlas },
							{ u8"textures",        rassets.get_images() },
							{ u8"positions",       scene.vertex_buffers },
							{ u8"normals",         scene.normal_buffers },
							{ u8"tangents",        scene.tangent_buffers },
							{ u8"uvs",             scene.uv_buffers },
							{ u8"indices",         scene.index_buffers },
							{ u8"instances",       scene.instances_buffer.bind_as_read_only() },
							{ u8"geometries",      scene.geometries_buffer.bind_as_read_only() },
							{ u8"materials",       scene.materials_buffer.bind_as_read_only() },
							{ u8"all_lights",      scene.lights_buffer.bind_as_read_only() },
						}
					);
					rctx.run_compute_shader_with_thread_dimensions(
						indirect_update_cs, probe_density, std::move(resources), u8"Update Indirect Probes"
					);
				}

				if (indirect_spatial_reuse) { // indirect spatial reuse
					uint32_t num_probes = probe_density[0] * probe_density[1] * probe_density[2];
					std::uint32_t num_indirect_reservoirs = num_probes * indirect_reservoirs_per_probe;
					auto new_indirect_reservoirs = rctx.request_structured_buffer<shader_types::indirect_lighting_reservoir>(
						u8"Indirect Lighting Reservoirs", num_indirect_reservoirs,
						lgpu::buffer_usage_mask::shader_read | lgpu::buffer_usage_mask::shader_write,
						runtime_buf_pool
					);

					cvec3i offsets[6] = {
						{  1,  0,  0 },
						{  0,  1,  0 },
						{  0,  0,  1 },
						{ -1,  0,  0 },
						{  0, -1,  0 },
						{  0,  0, -1 },
					};

					shader_types::indirect_spatial_reuse_constants reuse_constants;
					reuse_constants.offset = offsets[frame_index % 6];
					reuse_constants.frame_index = frame_index;
					reuse_constants.visibility_test_mode = indirect_spatial_reuse_visibility_test_mode;

					lren::all_resource_bindings resources(
						{},
						{
							{ u8"rtas",              scene.tlas },
							{ u8"input_reservoirs",  indirect_reservoirs.bind_as_read_only() },
							{ u8"output_reservoirs", new_indirect_reservoirs.bind_as_read_write() },
							{ u8"probe_consts",      lren_bds::immediate_constant_buffer::create_for(probe_constants) },
							{ u8"constants",         lren_bds::immediate_constant_buffer::create_for(reuse_constants) },
						}
					);
					rctx.run_compute_shader_with_thread_dimensions(indirect_spatial_reuse_cs, probe_density, std::move(resources), u8"Spatial Indirect Reuse");

					indirect_reservoirs = new_indirect_reservoirs;
				}

				{ // summarize probes
					lren::all_resource_bindings resources(
						{},
						{
							{ u8"indirect_reservoirs", indirect_reservoirs.bind_as_read_only() },
							{ u8"probe_sh",            probe_sh.bind_as_read_write() },
							{ u8"probe_consts",        lren_bds::immediate_constant_buffer::create_for(probe_constants) },
						}
					);
					rctx.run_compute_shader_with_thread_dimensions(
						summarize_probes_cs, probe_density, std::move(resources), u8"Summarize Probes"
					);
				}
			}

			{ // lighting
				lren::all_resource_bindings resources(
					{},
					{
						{ u8"gbuffer_albedo_glossiness", g_buf.albedo_glossiness.bind_as_read_only() },
						{ u8"gbuffer_normal",            g_buf.normal.bind_as_read_only() },
						{ u8"gbuffer_metalness",         g_buf.metalness.bind_as_read_only() },
						{ u8"gbuffer_depth",             g_buf.depth_stencil.bind_as_read_only() },
						{ u8"out_diffuse",               light_diffuse.bind_as_read_write() },
						{ u8"out_specular",              light_specular.bind_as_read_write() },
						{ u8"all_lights",                scene.lights_buffer.bind_as_read_only() },
						{ u8"direct_reservoirs",         direct_reservoirs.bind_as_read_only() },
						{ u8"indirect_probes",           probe_sh.bind_as_read_only() },
						{ u8"rtas",                      scene.tlas },
						{ u8"constants",                 lren_bds::immediate_constant_buffer::create_for(lighting_constants) },
						{ u8"probe_consts",              lren_bds::immediate_constant_buffer::create_for(probe_constants) },
					}
				);
				rctx.run_compute_shader_with_thread_dimensions(
					lighting_cs, cvec3u32(window_size.into<std::uint32_t>(), 1),
					std::move(resources), u8"Lighting"
				);
			}

			auto indirect_specular = rctx.request_image2d(u8"Indirect Specular", window_size, 1, lgpu::format::r32g32b32a32_float, lgpu::image_usage_mask::shader_read | lgpu::image_usage_mask::shader_write, runtime_tex_pool);

			{ // indirect specular
				shader_types::indirect_specular_constants constants;
				constants.enable_mis = enable_indirect_specular_mis;
				constants.frame_index = frame_index;
				lren::all_resource_bindings resources(
					{
						{ 8, rassets.get_samplers() },
					},
					{
						{ u8"probe_consts",              lren_bds::immediate_constant_buffer::create_for(probe_constants) },
						{ u8"constants",                 lren_bds::immediate_constant_buffer::create_for(constants) },
						{ u8"lighting_consts",           lren_bds::immediate_constant_buffer::create_for(lighting_constants) },
						{ u8"direct_probes",             direct_reservoirs.bind_as_read_only() },
						{ u8"indirect_probes",           indirect_reservoirs.bind_as_read_only() },
						{ u8"indirect_sh",               probe_sh.bind_as_read_only() },
						{ u8"out_specular",              indirect_specular.bind_as_read_write() },
						{ u8"rtas",                      scene.tlas },
						{ u8"gbuffer_albedo_glossiness", g_buf.albedo_glossiness.bind_as_read_only() },
						{ u8"gbuffer_normal",            g_buf.normal.bind_as_read_only() },
						{ u8"gbuffer_metalness",         g_buf.metalness.bind_as_read_only() },
						{ u8"gbuffer_depth",             g_buf.depth_stencil.bind_as_read_only() },
						{ u8"textures",                  rassets.get_images() },
						{ u8"positions",                 scene.vertex_buffers },
						{ u8"normals",                   scene.normal_buffers },
						{ u8"tangents",                  scene.tangent_buffers },
						{ u8"uvs",                       scene.uv_buffers },
						{ u8"indices",                   scene.index_buffers },
						{ u8"instances",                 scene.instances_buffer.bind_as_read_only() },
						{ u8"geometries",                scene.geometries_buffer.bind_as_read_only() },
						{ u8"materials",                 scene.materials_buffer.bind_as_read_only() },
						{ u8"all_lights",                scene.lights_buffer.bind_as_read_only() },
					}
				);

				rctx.run_compute_shader_with_thread_dimensions(
					indirect_specular_cs, cvec3u32(window_size.into<std::uint32_t>(), 1),
					std::move(resources), u8"Indirect Specular"
				);
			}

			if (shade_point_debug_mode != 0) {
				float tan_half_fovy = std::tan(0.5f * cam_params.fov_y_radians);
				cvec3f half_right = cam.unit_right * cam_params.aspect_ratio * tan_half_fovy;
				cvec3f half_down = cam.unit_up * -tan_half_fovy;
				cvec3f pixel_x = half_right / (0.5f * window_size[0]);
				cvec3f pixel_y = half_down / (0.5f * window_size[1]);

				shader_types::shade_point_debug_constants constants;
				constants.camera      = cvec4f(cam_params.position, 1.0f);
				constants.x           = cvec4f(pixel_x, 0.0f);
				constants.y           = cvec4f(pixel_y, 0.0f);
				constants.top_left    = cvec4f(cam.unit_forward - half_right - half_down, 0.0f);
				constants.window_size = window_size.into<std::uint32_t>();
				constants.num_lights  = scene.lights.size();
				constants.mode        = shade_point_debug_mode;
				constants.num_frames  = ++num_accumulated_frames;

				lren::all_resource_bindings resources(
					{
						{ 8, rassets.get_samplers() },
					},
					{
						{ u8"probe_consts",   lren_bds::immediate_constant_buffer::create_for(probe_constants) },
						{ u8"constants",      lren_bds::immediate_constant_buffer::create_for(constants) },
						{ u8"direct_probes",  direct_reservoirs.bind_as_read_only() },
						{ u8"indirect_sh",    probe_sh.bind_as_read_only() },
						{ u8"out_irradiance", light_diffuse.bind_as_read_write() },
						{ u8"out_accum",      path_tracer_accum.bind_as_read_write() },
						{ u8"rtas",           scene.tlas },
						{ u8"textures",       rassets.get_images() },
						{ u8"positions",      scene.vertex_buffers },
						{ u8"normals",        scene.normal_buffers },
						{ u8"tangents",       scene.tangent_buffers },
						{ u8"uvs",            scene.uv_buffers },
						{ u8"indices",        scene.index_buffers },
						{ u8"instances",      scene.instances_buffer.bind_as_read_only() },
						{ u8"geometries",     scene.geometries_buffer.bind_as_read_only() },
						{ u8"materials",      scene.materials_buffer.bind_as_read_only() },
						{ u8"all_lights",     scene.lights_buffer.bind_as_read_only() },
					}
				);
				rctx.run_compute_shader_with_thread_dimensions(
					shade_point_debug_cs, cvec3u32(window_size.into<std::uint32_t>(), 1),
					std::move(resources), u8"Shade Point Debug"
				);
			}

			{ // lighting combine
				lren::graphics_pipeline_state state(
					{ lgpu::render_target_blend_options::disabled() },
					nullptr,
					nullptr
				);
				shader_types::lighting_combine_constants constants;
				constants.lighting_scale = lighting_scale;
				constants.use_indirect_specular = use_indirect_specular;
				lren::all_resource_bindings resources(
					{
						{ 1, rassets.get_samplers() },
					},
					{
						{ u8"diffuse_lighting",  light_diffuse.bind_as_read_only() },
						{ u8"specular_lighting", light_specular.bind_as_read_only() },
						{ u8"indirect_specular", indirect_specular.bind_as_read_only() },
						{ u8"constants",         lren_bds::immediate_constant_buffer::create_for(constants) },
					}
				);

				auto pass = rctx.begin_pass(
					{ lren::image2d_color(swapchain, lgpu::color_render_target_access::create_discard_then_write()) },
					nullptr, window_size, u8"Lighting Combine Pass"
				);
				pass.draw_instanced(
					{}, 3, nullptr, 0, lgpu::primitive_topology::triangle_list,
					std::move(resources), fs_quad_vs, lighting_combine_ps, std::move(state), 1, u8"Lighting Combine");
				pass.end();
			}

			if (gbuffer_visualization > 0) {
				lren::graphics_pipeline_state state(
					{ lgpu::render_target_blend_options::disabled() },
					nullptr,
					nullptr
				);
				shader_types::gbuffer_visualization_constants constants;
				constants.mode = gbuffer_visualization;
				lren::all_resource_bindings resources(
					{
						{ 1, rassets.get_samplers() },
					},
					{
						{ u8"gbuffer_albedo_glossiness", g_buf.albedo_glossiness.bind_as_read_only() },
						{ u8"gbuffer_normal",            g_buf.normal.bind_as_read_only() },
						{ u8"gbuffer_metalness",         g_buf.metalness.bind_as_read_only() },
						{ u8"gbuffer_depth",             g_buf.depth_stencil.bind_as_read_only() },
						{ u8"constants",                 lren_bds::immediate_constant_buffer::create_for(constants) },
					}
				);

				auto pass = rctx.begin_pass(
					{ lren::image2d_color(swapchain, lgpu::color_render_target_access::create_discard_then_write()) },
					nullptr, window_size, u8"GBuffer Visualization Pass"
				);
				pass.draw_instanced(
					{}, 3, nullptr, 0, lgpu::primitive_topology::triangle_list,
					std::move(resources), fs_quad_vs, show_gbuffer_ps, std::move(state), 1, u8"GBuffer Visualization");
				pass.end();
			}

			if (visualize_probes_mode != 0) {
				lren::graphics_pipeline_state state(
					{ lgpu::render_target_blend_options::disabled() },
					nullptr,
					lgpu::depth_stencil_options(
						true, true, lgpu::comparison_function::greater,
						false, 0, 0, lgpu::stencil_options::always_pass_no_op(), lgpu::stencil_options::always_pass_no_op()
					)
				);

				shader_types::visualize_probes_constants constants;
				constants.projection_view = cam.projection_view_matrix;
				constants.unit_right      = cam.unit_right;
				constants.size            = visualize_probe_size;
				constants.unit_down       = cam.unit_up;
				constants.mode            = visualize_probes_mode;
				constants.unit_forward    = cam.unit_forward;
				constants.lighting_scale  = lighting_scale;

				lren::all_resource_bindings resources(
					{},
					{
						{ u8"probe_consts", lren_bds::immediate_constant_buffer::create_for(probe_constants) },
						{ u8"constants",    lren_bds::immediate_constant_buffer::create_for(constants) },
						{ u8"probe_values", probe_sh.bind_as_read_only() },
					}
				);

				std::uint32_t num_probes = probe_density[0] * probe_density[1] * probe_density[2];

				auto pass = rctx.begin_pass(
					{ lren::image2d_color(swapchain, lgpu::color_render_target_access::create_preserve_and_write()) },
					lren::image2d_depth_stencil(g_buf.depth_stencil, lgpu::depth_render_target_access::create_preserve_and_write()),
					window_size, u8"Probe Visualization Pass"
				);
				pass.draw_instanced(
					{}, 6, nullptr, 0, lgpu::primitive_topology::triangle_list,
					std::move(resources), visualize_probes_vs, visualize_probes_ps, std::move(state),
					num_probes, u8"Probe Visualization"
				);
				pass.end();
			}

			for (const auto &l : scene.lights) {
				debug_render.add_locator(l.position, lotus::linear_rgba_f(1.0f, 0.0f, 0.0f, 1.0f));
			}

			{ // debug drawing
				debug_render.flush(
					lren::image2d_color(swapchain, lgpu::color_render_target_access::create_preserve_and_write()),
					lren::image2d_depth_stencil(g_buf.depth_stencil, lgpu::depth_render_target_access::create_preserve_and_write()),
					window_size, cam.projection_view_matrix
				);
			}

			{ // dear ImGui
				ImGui::NewFrame();

				bool needs_resizing = false;

				if (ImGui::Begin("Controls")) {
					ImGui::SliderFloat("Lighting Scale", &lighting_scale, 0.01f, 100.0f, "%.02f", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoRoundToFormat);
					ImGui::Combo("Show G-Buffer", &gbuffer_visualization, "Disabled\0Albedo\0Glossiness\0Normal\0Metalness\0Emissive\0");
					ImGui::Checkbox("Update Probes", &update_probes);
					if (ImGui::Button("Update Probes This Frame")) {
						update_probes_this_frame = true;
					}
					ImGui::Checkbox("Trace Naive Shadow Rays", &trace_shadow_rays_naive);
					ImGui::Checkbox("Trace Reservoir Shadow Rays", &trace_shadow_rays_reservoir);
					ImGui::Combo("Lighting Mode", &lighting_mode, "None\0Reservoir\0Naive\0");
					ImGui::SliderFloat("Direct Diffuse Multiplier", &diffuse_mul, 0.0f, 1.0f);
					ImGui::SliderFloat("Direct Specular Multiplier", &specular_mul, 0.0f, 1.0f);
					ImGui::Checkbox("Show Indirect Diffuse", &use_indirect_diffuse);
					ImGui::Checkbox("Show Indirect Specular", &use_indirect_specular);
					ImGui::Checkbox("Use Indirect Specular MIS", &enable_indirect_specular_mis);
					if (ImGui::Combo("Shade Point Debug Mode", &shade_point_debug_mode, "Off\0Lighting\0Albedo\0Normal\0Path Tracer\0")) {
						num_accumulated_frames = 0;
					}
					ImGui::Separator();

					ImGui::Combo("Visualize Probes", &visualize_probes_mode, "None\0Specular\0Diffuse\0Normal\0");
					ImGui::SliderFloat("Visualize Probes Size", &visualize_probe_size, 0.0f, 1.0f);
					if (ImGui::Button("Reset Probes")) {
						needs_resizing = true;
					}
					{
						auto probes_int = probe_density.into<int>();
						int probes[3] = { probes_int[0], probes_int[1], probes_int[2] };
						needs_resizing = ImGui::SliderInt3("Num Probes", probes, 2, 100) || needs_resizing;
						probe_density = cvec3u32(probes[0], probes[1], probes[2]);
					}
					needs_resizing = ImGui_SliderT<std::uint32_t>("Direct Reservoirs Per Probe", &direct_reservoirs_per_probe, 1, 20) || needs_resizing;
					needs_resizing = ImGui_SliderT<std::uint32_t>("Indirect Reservoirs Per Probe", &indirect_reservoirs_per_probe, 1, 20) || needs_resizing;
					{
						float rx[2] = { probe_bounds.min[0], probe_bounds.max[0] };
						float ry[2] = { probe_bounds.min[1], probe_bounds.max[1] };
						float rz[2] = { probe_bounds.min[2], probe_bounds.max[2] };
						needs_resizing = ImGui::SliderFloat2("Range X", rx, -20.0f, 20.0f) || needs_resizing;
						needs_resizing = ImGui::SliderFloat2("Range Y", ry, -20.0f, 20.0f) || needs_resizing;
						needs_resizing = ImGui::SliderFloat2("Range Z", rz, -20.0f, 20.0f) || needs_resizing;
						probe_bounds = lotus::aab3f::create_from_min_max({ rx[0], ry[0], rz[0] }, { rx[1], ry[1], rz[1] });
					}
					ImGui::Checkbox("Indirect Spatial Reuse", &indirect_spatial_reuse);
					ImGui::Combo("Indirect Spatial Reuse Visibility Test Mode", &indirect_spatial_reuse_visibility_test_mode, "None\0Simple\0Full\0");
					ImGui_SliderT<std::uint32_t>("Direct Sample Count Cap", &direct_sample_count_cap, 1, 10000, "%d", ImGuiSliderFlags_Logarithmic);
					ImGui_SliderT<std::uint32_t>("Indirect Sample Count Cap", &indirect_sample_count_cap, 1, 10000, "%d", ImGuiSliderFlags_Logarithmic);
				}
				ImGui::End();

				if (needs_resizing) {
					resize_probe_buffers();
				}

				ImGui::Render();
				imgui_rctx.render(
					lren::image2d_color(swapchain, lgpu::color_render_target_access::create_preserve_and_write()),
					window_size, runtime_buf_pool
				);
			}

			rctx.present(swapchain, u8"Present");
		}

		rctx.flush();
		++frame_index;
	}

	return 0;
}