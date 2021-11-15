#pragma once

/// \file
/// Stack allocator.

#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#include "lotus/common.h"
#include "lotus/memory.h"

namespace lotus {
	/// An allocator that allocates out of a stack. The user can make bookmarks in the stack that the allocator can
	/// unwind to.
	class stack_allocator {
	protected:
		struct _page_header;
		struct _bookmark;
	public:
		/// Whether or not to poision memory that has been freed.
		constexpr static bool poison_freed_memory = is_debugging;

		/// A STL container compatible allocator for \ref stack_allocator.
		template <typename T> class allocator {
			template <typename U> friend class allocator;
		public:
			using value_type = T; ///< Value type.

			/// Creates an empty (and invalid) allocator.
			allocator(std::nullptr_t) {
			}
			/// Creates an allocator for the given \ref stack_allocator.
			[[nodiscard]] inline static allocator create_for(stack_allocator &alloc) {
				allocator result = nullptr;
				result._alloc = &alloc;
				return result;
			}
			/// Conversion from an allocator of another type.
			template <typename U> explicit allocator(const allocator<U> &src) :
				_alloc(src._alloc) {
			}

			/// Allocates an array.
			[[nodiscard]] T *allocate(std::size_t n) const {
				return static_cast<T*>(_alloc->_allocate(sizeof(T) * n, alignof(T)));
			}
			/// Does nothing. De-allocation only happens when popping bookmarks.
			void deallocate(T*, std::size_t) const {
			}

			/// If two objects are on the same thread, they use the same underlying allocator.
			friend bool operator==(const allocator&, const allocator&) = default;
		protected:
			stack_allocator *_alloc; ///< The underlying allocator.
		};
		/// Vector type that uses a stack allocator.
		template <typename T> using vector_type = std::vector<T, allocator<T>>;
		/// String type that uses a stack allocator.
		template <typename Ch, typename Traits = std::char_traits<Ch>> using string_type =
			std::basic_string<Ch, Traits, allocator<Ch>>;
		/// An RAII bookmark.
		struct scoped_bookmark {
			friend stack_allocator;
		public:
			/// Creates an empty bookmark.
			scoped_bookmark(std::nullptr_t) : _alloc(nullptr) {
			}
			/// Move construction.
			scoped_bookmark(scoped_bookmark &&src) : _alloc(std::exchange(src._alloc, nullptr)) {
			}
			/// Move assignment.
			scoped_bookmark &operator=(scoped_bookmark &&src) {
				if (this != &src) {
					reset();
					_alloc = std::exchange(src._alloc, nullptr);
				}
				return *this;
			}
			/// Calls \ref pop_bookmark().
			~scoped_bookmark() {
				reset();
			}

			/// Allocates a piece of memory from the current segment.
			[[nodiscard]] void *allocate(std::size_t size, std::size_t align) {
				assert(_alloc && _alloc->_top_bookmark == _this_bookmark);
				return _alloc->_allocate(size, align);
			}
			/// Allocates memory for an object or an array of objects.
			template <typename T> [[nodiscard]] T *allocate(std::size_t count = 1) {
				return static_cast<T*>(allocate(sizeof(T) * count, alignof(T)));
			}

			/// Creates a \ref allocator for the given type.
			template <typename T> [[nodiscard]] allocator<T> create_std_allocator() {
				assert(_alloc && _alloc->_top_bookmark == _this_bookmark);
				return allocator<T>::create_for(*_alloc);
			}
			/// Convenience function for creating a \p std::vector using the given parameters and this allocator.
			template <
				typename T, typename ...Args
			> [[nodiscard]] vector_type<T> create_vector_array(Args &&...args) {
				return vector_type<T>(std::forward<Args>(args)..., create_std_allocator<T>());
			}
			/// Convenience function for creating a \p std::vector with the specified reserved space using the given
			/// parameters and this allocator.
			template <
				typename T, typename ...Args
			> [[nodiscard]] vector_type<T> create_reserved_vector_array(std::size_t capacity) {
				vector_type<T> result(create_std_allocator<T>());
				result.reserve(capacity);
				return result;
			}
			/// Convenience function for creating a \p std::basic_string using the given parameters and this
			/// allocator.
			template <
				typename Ch, typename Traits = std::char_traits<Ch>, typename ...Args
			> [[nodiscard]] string_type<Ch, Traits> create_string(Args &&...args) {
				return string_type<Ch, Traits>(std::forward<Args>(args)..., create_std_allocator<Ch>());
			}
			/// Convenience function for creating a \p std::u8string using the given parameters and this allocator.
			template <typename ...Args> [[nodiscard]] string_type<char8_t> create_u8string(Args &&...args) {
				return string_type<char8_t>(std::forward<Args>(args)..., create_std_allocator<char8_t>());
			}

			/// Resets this object, popping the bookmark if necessary.
			void reset() {
				if (_alloc) {
					_alloc->_pop_bookmark();
					_alloc = nullptr;
				}
			}
		protected:
			stack_allocator *_alloc; ///< The allocator;
			_bookmark *_this_bookmark; ///< Position of this bookmark used for debugging.

			/// Creates an object for the given allocator and sets a bookmark.
			explicit scoped_bookmark(stack_allocator &alloc) : _alloc(&alloc) {
				_alloc->_set_bookmark();
				_this_bookmark = _alloc->_top_bookmark;
			}
		};

		/// Default constructor.
		stack_allocator() = default;
		/// No copy construction.
		stack_allocator(const stack_allocator&) = delete;
		/// No copy assignment.
		stack_allocator &operator=(const stack_allocator&) = delete;
		/// Frees all pages.
		~stack_allocator();

		/// Creates a bookmark and returns it.
		[[nodiscard]] scoped_bookmark bookmark() {
			return scoped_bookmark(*this);
		}

		/// Frees all pages in \ref _free_pages.
		void free_unused_pages();

		/// Returns the \ref stack_allocator for this thread.
		static stack_allocator &for_this_thread();

		std::size_t page_size = 8 * 1024 * 1024; /// Size of a page.
		void *(*allocate_page)(std::size_t, std::size_t) = memory::raw::allocate; ///< Used to allocate the pages.
		void (*free_page)(void*) = memory::raw::free; ///< Used to free a page.
	protected:
		/// Reference to a page.
		struct _page_ref {
			/// No initialization.
			_page_ref(uninitialized_t) {
			}
			/// Initializes this reference to empty.
			_page_ref(std::nullptr_t) : memory(nullptr), header(nullptr), current(nullptr), end(nullptr) {
			}
			/// Creates a new reference to the given newly allocated page. \ref header is not initialized.
			[[nodiscard]] static _page_ref to_new_page(void*, std::size_t sz);

			/// Allocates a block of memory from this page. If there's not enough space within this page, this
			/// function returns \p nullptr. The returned memory block is not initialized.
			[[nodiscard]] void *allocate(std::size_t size, std::size_t align);
			/// \overload
			template <typename T> [[nodiscard]] T *allocate() {
				return static_cast<T*>(allocate(sizeof(T), alignof(T)));
			}

			/// Calls the destructor of \ref header, then empties this page and re-allocate the header.
			void reset(_page_header new_header) {
				header->~_page_header();
				current = memory;
				header = new (allocate<_page_header>()) _page_header(new_header);
			}

			/// Poisons all bytes in the page after the given pointer.
			void poison_after(void *ptr) {
				assert(ptr >= memory && ptr <= end);
				std::memset(ptr, 0xCD, static_cast<std::byte*>(end) - static_cast<std::byte*>(ptr));
			}

			/// Tests if this reference is empty.
			[[nodiscard]] explicit operator bool() const {
				return memory != nullptr;
			}

			void *memory; ///< Pointer to the memory block.
			_page_header *header; ///< The header of this page.
			void *current; ///< Next byte that could be allocated.
			void *end; ///< Pointer past the page.
		};
		/// Header of a page.
		struct _page_header {
			/// No initialization.
			_page_header(uninitialized_t) {
			}
			/// Creates a header object with the given reference to the previous page.
			[[nodiscard]] static _page_header create(_page_ref prev, void (*free)(void*));

			_page_ref previous = uninitialized; ///< The previous page.
			void (*free_page)(void*); ///< The function that should be used to free this page.
		};
		/// Bookmark data.
		struct _bookmark {
			/// No initialization.
			_bookmark(uninitialized_t) {
			}
			/// Creates a new bookmark object.
			[[nodiscard]] static _bookmark create(void *page, void *cur, _bookmark *prev);

			void *page; ///< Address of the page that this bookmark is in.
			void *current; ///< Position of the bookmark within the page.
			_bookmark *previous; ///< The previous bookmark.
		};

		/// Creates a new page and allocates a \ref _page_ref at the front to the current top page.
		[[nodiscard]] _page_ref _allocate_new_page(_page_ref prev, std::size_t size) const {
			auto result = _page_ref::to_new_page(allocate_page(size, alignof(_page_header)), size);
			result.header = new (result.allocate<_page_header>()) _page_header(_page_header::create(prev, free_page));
			return result;
		}
		/// \overload
		[[nodiscard]] _page_ref _allocate_new_page(_page_ref prev) const {
			return _allocate_new_page(prev, page_size);
		}

		/// Sets a new bookmark.
		void _set_bookmark() {
			auto mark = _bookmark::create(_top_page.memory, _top_page.current, _top_bookmark);
			_top_bookmark = new (_allocate(sizeof(_bookmark), alignof(_bookmark))) _bookmark(mark);
		}
		/// Resets the allocator to the state before the last bookmark was allocated. All allocated memory since then
		/// must be properly freed by this point.
		void _pop_bookmark();

		/// Allocates a new block of memory.
		void *_allocate(std::size_t size, std::size_t align);

		/// Replaces \ref _top_page with a new page. If \ref _free_pages is empty, a page is taken from the list;
		/// otherwise a new page is allocated.
		void _take_page();
		/// Assumes that \ref _top_page is empty and returns it to \ref _free_pages.
		void _return_page();

		_page_ref _top_page = nullptr; ///< The page currently in use.
		/// A list of free pages. All pages in this list have correct \ref _page_ref::current fields (i.e., only
		/// accounting for the header).
		_page_ref _free_pages = nullptr;
		_bookmark *_top_bookmark = nullptr; ///< The most recent bookmark.
	};
}
