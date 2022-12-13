#pragma once

/// \file
/// Simple scene loader and storage.

#include <lotus/renderer/loaders/assimp_loader.h>
#include <lotus/renderer/loaders/gltf_loader.h>

#include "lotus.h"

/// Stores the representation of a scene.
class scene_representation {
public:
	explicit scene_representation(lren::assets::manager &assman) : _assets(assman) {
		auto &rctx = _assets.get_context();

		vertex_buffers = rctx.request_buffer_descriptor_array(u8"Vertex buffers", lgpu::descriptor_type::read_only_buffer, 16384);
		normal_buffers = rctx.request_buffer_descriptor_array(u8"Normal buffers", lgpu::descriptor_type::read_only_buffer, 16384);
		tangent_buffers = rctx.request_buffer_descriptor_array(u8"Tangent buffers", lgpu::descriptor_type::read_only_buffer, 16384);
		uv_buffers = rctx.request_buffer_descriptor_array(u8"UV buffers", lgpu::descriptor_type::read_only_buffer, 16384);
		index_buffers = rctx.request_buffer_descriptor_array(u8"Index buffers", lgpu::descriptor_type::read_only_buffer, 16384);

		geom_buffer_pool = rctx.request_pool(u8"Geometry Buffers", rctx.get_device_memory_type_index());
		geom_texture_pool = rctx.request_pool(u8"Geometry Textures", rctx.get_device_memory_type_index());
		as_pool = rctx.request_pool(u8"Acceleration Structures", rctx.get_device_memory_type_index());
	}

	void on_texture_loaded(lren::assets::handle<lren::assets::image2d> tex) {
		/*mip_gen.generate_all(tex->image);*/
	}
	void on_geometry_loaded(lren::assets::handle<lren::assets::geometry> geom) {
		auto &rctx = _assets.get_context();

		geom.user_data() = reinterpret_cast<void*>(static_cast<std::uintptr_t>(blases.size()));

		auto &blas = blases.emplace_back(rctx.request_blas(
			geom.get().get_id().subpath, { geom->get_geometry_buffers_view(lgpu::raytracing_geometry_flags::opaque) }, as_pool
		));
		rctx.build_blas(blas, u8"Build BLAS");

		auto &inst = geometries.emplace_back();
		if (geom->index_buffer) {
			inst.index_buffer = _index_alloc++;
			rctx.write_buffer_descriptors(index_buffers, inst.index_buffer, {
				geom->index_buffer->data.get_view(
					geom->index_format == lgpu::index_format::uint16 ?
						sizeof(std::uint16_t) : sizeof(std::uint32_t),
					geom->index_offset,
					geom->num_indices
				)
			});
		}
		inst.vertex_buffer = inst.normal_buffer = inst.tangent_buffer = inst.uv_buffer = _buffer_alloc++;
		rctx.write_buffer_descriptors(vertex_buffers, inst.vertex_buffer, {
			geom->vertex_buffer.data->data.get_view(
				geom->vertex_buffer.stride, geom->vertex_buffer.offset, geom->num_vertices
			)
		});
		if (geom->normal_buffer.data) {
			rctx.write_buffer_descriptors(normal_buffers, inst.normal_buffer, {
				geom->normal_buffer.data->data.get_view(
					geom->normal_buffer.stride, geom->normal_buffer.offset, geom->num_vertices
				)
			});
		}
		if (geom->tangent_buffer.data) {
			rctx.write_buffer_descriptors(tangent_buffers, inst.tangent_buffer, {
				geom->tangent_buffer.data->data.get_view(
					geom->tangent_buffer.stride, geom->tangent_buffer.offset, geom->num_vertices
				)
			});
		} else {
			inst.tangent_buffer = std::numeric_limits<std::uint32_t>::max();
		}
		if (geom->uv_buffer.data) {
			rctx.write_buffer_descriptors(uv_buffers, inst.uv_buffer, {
				geom->uv_buffer.data->data.get_view(
					geom->uv_buffer.stride, geom->uv_buffer.offset, geom->num_vertices
				)
			});
		}
	}
	void on_material_loaded(lren::assets::handle<lren::assets::material> mat) {
		mat.user_data() = reinterpret_cast<void*>(static_cast<std::uintptr_t>(materials.size()));
		auto &mat_data = materials.emplace_back();
		if (auto *data = dynamic_cast<lren::generic_pbr_material_data*>(mat->data.get())) {
			std::uint32_t invalid_tex = _assets.get_invalid_image()->descriptor_index;
			mat_data.assets.albedo_texture = data->albedo_texture ? data->albedo_texture->descriptor_index : invalid_tex;
			mat_data.assets.normal_texture = data->normal_texture ? data->normal_texture->descriptor_index : invalid_tex;
			mat_data.assets.properties_texture = data->properties_texture ? data->properties_texture->descriptor_index : invalid_tex;
			mat_data.assets.properties2_texture = invalid_tex;
			mat_data.properties = data->properties;
			/*mat_data.is_metallic_roughness = true;*/ // TODO
		}
		material_assets.emplace_back(std::move(mat));
	}
	void on_instance_loaded(lren::instance inst) {
		if (inst.geometry) {
			/*// hack to not load decals
			if (inst.geometry.get().get_id().subpath.find(u8"decal") != std::u8string::npos) {
				return;
			}*/

			auto geom_index = reinterpret_cast<std::uintptr_t>(inst.geometry.user_data());
			auto mat_index = inst.material ? reinterpret_cast<std::uintptr_t>(inst.material.user_data()) : 0;
			auto inst_index = instances.size();
			tlas_instances.emplace_back(
				blases[geom_index],
				inst.transform,
				static_cast<std::uint32_t>(inst_index),
				0xFFu,
				inst.geometry->index_buffer ? 0u : 1u,
				lgpu::raytracing_instance_flags::none
			);
			instances.emplace_back(std::move(inst));
			auto &gpu_inst = instance_data.emplace_back();
			gpu_inst.geometry_index = geom_index;
			gpu_inst.material_index = mat_index;
			auto tangent_trans = inst.transform.block<3, 3>(0, 0);
			auto decomp = lotus::mat::lup_decompose(inst.transform.block<3, 3>(0, 0).into<double>());
			gpu_inst.determinant = std::pow(decomp.determinant(), 1.0f / 3.0f);
			mat44f normal_trans = zero;
			normal_trans.set_block(0, 0, (decomp.invert().transposed() * gpu_inst.determinant).into<float>());
			gpu_inst.normal_transform = normal_trans;
		}
	}
	void on_light_loaded(lren::shader_types::light l) {
		lights.emplace_back(l);
	}

	void load(const std::filesystem::path &path) {
		if (path.extension() == ".gltf") {
			lren::gltf::context ctx(_assets);
			ctx.load(
				path,
				[this](lren::assets::handle<lren::assets::image2d> h) { on_texture_loaded(std::move(h)); },
				[this](lren::assets::handle<lren::assets::geometry> h) { on_geometry_loaded(std::move(h)); },
				[this](lren::assets::handle<lren::assets::material> h) { on_material_loaded(std::move(h)); },
				[this](lren::instance h) { on_instance_loaded(std::move(h)); },
				[this](lren::shader_types::light l) { on_light_loaded(l); },
				geom_buffer_pool,
				geom_texture_pool
			);
		} else {
			lren::assimp::context ctx(_assets);
			ctx.load(
				path,
				[this](lren::assets::handle<lren::assets::image2d> h) { on_texture_loaded(std::move(h)); },
				[this](lren::assets::handle<lren::assets::geometry> h) { on_geometry_loaded(std::move(h)); },
				[this](lren::assets::handle<lren::assets::material> h) { on_material_loaded(std::move(h)); },
				[this](lren::instance h) { on_instance_loaded(std::move(h)); },
				[this](lren::shader_types::light l) { on_light_loaded(l); },
				geom_buffer_pool,
				geom_texture_pool
			);
		}
	}
	void finish_loading() {
		auto &rctx = _assets.get_context();

		tlas = rctx.request_tlas(u8"TLAS", tlas_instances, as_pool);
		rctx.build_tlas(tlas, u8"Build TLAS");

		auto geom_buf = rctx.request_buffer(
			u8"Geometry buffer",
			sizeof(shader_types::geometry_data) * geometries.size(),
			lgpu::buffer_usage_mask::copy_destination | lgpu::buffer_usage_mask::shader_read_only,
			geom_buffer_pool
		);
		rctx.upload_buffer<shader_types::geometry_data>(geom_buf, geometries, 0, u8"Upload geometry buffer");
		geometries_buffer = geom_buf.get_view<shader_types::geometry_data>(0, geometries.size());

		if (materials.empty()) {
			materials.emplace_back();
		}
		auto mat_buf = rctx.request_buffer(
			u8"Material buffer",
			sizeof(lren::shader_types::generic_pbr_material::material) * materials.size(),
			lgpu::buffer_usage_mask::copy_destination | lgpu::buffer_usage_mask::shader_read_only,
			geom_buffer_pool
		);
		rctx.upload_buffer<lren::shader_types::generic_pbr_material::material>(mat_buf, materials, 0, u8"Upload material buffer");
		materials_buffer = mat_buf.get_view<lren::shader_types::generic_pbr_material::material>(0, materials.size());

		if (instance_data.empty()) {
			instance_data.emplace_back();
		}
		auto inst_buf = rctx.request_buffer(
			u8"Instance buffer",
			sizeof(shader_types::rt_instance_data) * instance_data.size(),
			lgpu::buffer_usage_mask::copy_destination | lgpu::buffer_usage_mask::shader_read_only,
			geom_buffer_pool
		);
		rctx.upload_buffer<shader_types::rt_instance_data>(inst_buf, instance_data, 0, u8"Upload instance buffer");
		instances_buffer = inst_buf.get_view<shader_types::rt_instance_data>(0, instance_data.size());

		if (lights.empty()) {
			lights.emplace_back();
		}
		auto light_buf = rctx.request_buffer(
			u8"Light buffer",
			sizeof(lren::shader_types::light) * lights.size(),
			lgpu::buffer_usage_mask::copy_destination | lgpu::buffer_usage_mask::shader_read_only,
			geom_buffer_pool
		);
		rctx.upload_buffer<lren::shader_types::light>(light_buf, lights, 0, u8"Upload lights buffer");
		lights_buffer = light_buf.get_view<lren::shader_types::light>(0, lights.size());
	}

	lren::pool geom_buffer_pool = nullptr;
	lren::pool geom_texture_pool = nullptr;
	lren::pool as_pool = nullptr;

	std::vector<lren::instance> instances;
	std::vector<lren::blas_reference> tlas_instances;
	std::vector<lren::assets::handle<lren::assets::material>> material_assets;
	std::vector<lren::shader_types::generic_pbr_material::material> materials;
	std::vector<lren::shader_types::light> lights;
	std::vector<lren::blas> blases;
	lren::tlas tlas = nullptr;

	lren::buffer_descriptor_array vertex_buffers = nullptr;
	lren::buffer_descriptor_array normal_buffers = nullptr;
	lren::buffer_descriptor_array tangent_buffers = nullptr;
	lren::buffer_descriptor_array uv_buffers = nullptr;
	lren::buffer_descriptor_array index_buffers = nullptr;

	std::vector<shader_types::rt_instance_data> instance_data;
	std::vector<shader_types::geometry_data> geometries;

	lren::structured_buffer_view geometries_buffer = nullptr;
	lren::structured_buffer_view materials_buffer = nullptr;
	lren::structured_buffer_view instances_buffer = nullptr;
	lren::structured_buffer_view lights_buffer = nullptr;
private:
	lren::assets::manager &_assets;

	std::uint32_t _buffer_alloc = 0;
	std::uint32_t _index_alloc = 0;
};
