#pragma once

/// \file
/// DirectX shader compiler interface.

#include <filesystem>
#include <span>

#include "lotus/gpu/common.h"
#include "details.h"
#include "dxil_reflection.h"

namespace lotus::gpu::backends::common {
	/// DXC compiler.
	struct dxc_compiler {
	public:
		/// Default extra arguments added when compiling shaders.
		const static std::initializer_list<LPCWSTR> default_extra_arguments;

		/// Contains a \p IDxcResult.
		struct compilation_result {
			friend dxc_compiler;
		public:
			/// Returns whether the result of \p IDxcResult::GetStatus() is success.
			[[nodiscard]] bool succeeded() const;
			/// Caches \ref _output if necessary, and returns it.
			[[nodiscard]] std::u8string_view get_compiler_output();
			/// Caches \ref _binary if necessary, and returns it.
			[[nodiscard]] std::span<const std::byte> get_compiled_binary();

			/// Returns a reference to the raw \p IDxcResult.
			[[nodiscard]] IDxcResult &get_result() const {
				return *_result;
			}
		private:
			_details::com_ptr<IDxcResult> _result; ///< Result.
			_details::com_ptr<IDxcBlob> _binary; ///< Cached compiled binary.
			_details::com_ptr<IDxcBlobUtf8> _messages; ///< Cached compiler output.
		};

		/// No initialization.
		dxc_compiler(std::nullptr_t) {
		}

		/// Calls \p IDxcCompiler3::Compile().
		[[nodiscard]] compilation_result compile_shader(
			std::span<const std::byte>, shader_stage, std::u8string_view entry_point,
			const std::filesystem::path &shader_path, std::span<const std::filesystem::path> include_paths,
			std::span<const std::pair<std::u8string_view, std::u8string_view>> defines,
			std::span<const LPCWSTR> args
		);
		/// \overload
		[[nodiscard]] compilation_result compile_shader(
			std::span<const std::byte> code, shader_stage stage, std::u8string_view entry_point,
			const std::filesystem::path &shader_path, std::span<const std::filesystem::path> include_paths,
			std::span<const std::pair<std::u8string_view, std::u8string_view>> defines,
			std::initializer_list<LPCWSTR> args
		) {
			return compile_shader(
				code, stage, entry_point, shader_path, include_paths, defines, { args.begin(), args.end() }
			);
		}

		/// Calls \p IDxcCompiler3::Compile().
		[[nodiscard]] compilation_result compile_shader_library(
			std::span<const std::byte> code,
			const std::filesystem::path &shader_path, std::span<const std::filesystem::path> include_paths,
			std::span<const std::pair<std::u8string_view, std::u8string_view>> defines,
			std::span<const LPCWSTR> args
		);
		/// \overload
		[[nodiscard]] compilation_result compile_shader_library(
			std::span<const std::byte> code,
			const std::filesystem::path &shader_path, std::span<const std::filesystem::path> include_paths,
			std::span<const std::pair<std::u8string_view, std::u8string_view>> defines,
			std::initializer_list<const LPCWSTR> args
		) {
			return compile_shader_library(code, shader_path, include_paths, defines, { args.begin(), args.end() });
		}

		/// Loads shader reflection using \p IDxcContainerReflection::GetPartReflection().
		void load_shader_reflection(std::span<const std::byte> data, REFIID iid, void **ppvObject);
		/// Loads a shader reflection for a \p ID3D12ShaderReflection.
		[[nodiscard]] dxil_reflection load_shader_reflection(std::span<const std::byte> data);
		/// Loads a shader reflection for a \p ID3D12LibraryReflection.
		[[nodiscard]] dxil_library_reflection load_shader_library_reflection(std::span<const std::byte> data);

		/// Initializes \ref _dxc_utils if necessary, and returns it.
		[[nodiscard]] IDxcUtils &get_utils();
		/// Initializes \ref _dxc_compiler if necessary, and returns it.
		[[nodiscard]] IDxcCompiler3 &get_compiler();
		/// Initializes \ref _dxc_include_handler if necessary, and returns it.
		[[nodiscard]] IDxcIncludeHandler &get_include_handler();
	private:
		_details::com_ptr<IDxcUtils> _dxc_utils; ///< Lazy-initialized DXC library handle.
		_details::com_ptr<IDxcCompiler3> _dxc_compiler; ///< Lazy-initialized DXC compiler.
		/// Lazy-initialized default DXC include handler.
		_details::com_ptr<IDxcIncludeHandler> _dxc_include_handler;

		/// Calls \p IDxcCompiler3::Compile().
		[[nodiscard]] compilation_result _do_compile_shader(
			std::span<const std::byte> code,
			const std::filesystem::path &shader_path, std::span<const std::filesystem::path> include_paths,
			std::span<const std::pair<std::u8string_view, std::u8string_view>> defines,
			std::span<const LPCWSTR> extra_args,
			std::initializer_list<LPCWSTR> extra_args_2,
			std::span<const LPCWSTR> extra_args_3
		);
	};
}
