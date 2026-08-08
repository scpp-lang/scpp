// scpp_string_wrapper.h
//
// A plain C ABI (`extern "C"`) wrapper around real C++ `std::string`,
// consumed by libs/std/string/String.cpp's scpp `class String`. This is the
// concrete demonstration of scpp calling into a real C/C++ library (see
// libs/README.md): the wrapper is compiled by an ordinary C++
// compiler (clang++/g++) into a small static library, entirely independent
// of the scpp toolchain, and scpp code links against it like any other
// native library.
//
// Each function operates on an opaque `void*` handle -- the address of a
// heap-allocated `std::string` -- so no C++ type (std::string itself, or
// any name from namespace std) ever needs to cross the extern "C" boundary;
// scpp only ever sees a `void*` and plain scalar/`char*` types it already
// understands.
#ifndef SCPP_STRING_WRAPPER_H
#define SCPP_STRING_WRAPPER_H

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// Allocates a new std::string, copy-initialized from the given
// nul-terminated C string (an empty string if `s` is NULL). Returns an
// opaque handle; ownership transfers to the caller, who must eventually
// pass it to scpp_string_delete exactly once.
void* scpp_string_new(const char* s);
void* scpp_string_new_sized(const char* s, std::size_t n);

// Allocates a new std::string whose content is a deep copy of `handle`'s
// string value. Returns a distinct owning handle that the caller must later
// pass to scpp_string_delete exactly once.
void* scpp_string_copy(const void* handle);

// Destroys a handle previously returned by scpp_string_new. `handle` must
// not be used again afterward (matches std::string's own destructor
// semantics -- this *is* that destructor, just called through a plain
// function instead of C++'s implicit mechanism).
void scpp_string_delete(void* handle);

// Returns the string's length in bytes (std::string::size()).
std::size_t scpp_string_length(void* handle);

// Requests that the string's capacity be at least `n` bytes
// (std::string::reserve(n)) -- a pure performance hint with no
// observable effect on the string's content/length, forwarded directly
// onto real std::string::reserve.
void scpp_string_reserve(void* handle, std::size_t n);

// Returns a pointer to the string's internal nul-terminated buffer
// (std::string::c_str()). Valid only until the next mutating call
// (scpp_string_append) or scpp_string_delete on the same handle -- same
// invalidation rule as real std::string::c_str().
const char* scpp_string_c_str(void* handle);

// Appends `s` to this string's content in place (a no-op if `s`
// is NULL).
void scpp_string_append(void* handle, const char* s);

// Appends a single character to this string's content in place
// (std::string::push_back(c)).
void scpp_string_push_back(void* handle, char c);

// Returns 1 if `handle`'s content equals the nul-terminated C string `s`,
// 0 otherwise (including when `s` is NULL).
int scpp_string_equals(void* handle, const char* s);

// Returns 1 if `handle`'s content contains `needle` as a substring
// (std::string::contains(std::string_view), C++23), 0 otherwise
// (including when `needle` is NULL, matching the convention every other
// wrapper here uses for a NULL C-string argument).
int scpp_string_contains(void* handle, const char* needle);

// Returns the byte offset of the first occurrence of `needle` in
// `handle`'s content at or after byte offset `start` (std::string::find),
// or scpp_string_npos() if `needle` does not occur at or after `start`
// (including when `needle` is NULL). A `start` past the end of the
// string is treated the same as real std::string::find: never matches
// (yields scpp_string_npos()) rather than aborting.
std::size_t scpp_string_find(void* handle, const char* needle, std::size_t start);
std::size_t scpp_string_find_char(void* handle, char needle, std::size_t start);

// Returns the byte offset of the LAST occurrence of `needle` in
// `handle`'s content (std::string::rfind, searching backward from the
// end), or scpp_string_npos() if absent (including when `needle` is
// NULL).
std::size_t scpp_string_rfind(void* handle, const char* needle);
std::size_t scpp_string_rfind_char(void* handle, char needle);

// Returns 1 if `handle`'s content ends with the nul-terminated C string
// `suffix` (std::string::ends_with, C++20), 0 otherwise (including when
// `suffix` is NULL).
int scpp_string_ends_with(void* handle, const char* suffix);

// Allocates a NEW std::string holding the substring of `handle`'s
// content starting at byte offset `pos`, `len` bytes long (silently
// clamped to however many bytes are actually available, same as real
// std::string::substr's own clamping of its `count` argument). Returns
// a pointer to an internal, thread-local buffer holding the result --
// valid only until the next call to scpp_string_substr on the same
// thread (same "valid until next call" invalidation rule as
// scpp_int64_to_cstr/scpp_uint64_to_cstr). Aborts if `pos` is past the
// end of the string, mirroring std::string::substr's std::out_of_range
// adapted to scpp's no-exceptions model (same convention as e.g.
// std::vector's own __check_index).
const char* scpp_string_substr(void* handle, std::size_t pos, std::size_t len);

// Formats `value` as a decimal string and returns a pointer to an
// internal, thread-local buffer holding the result -- valid only until
// the next call to scpp_int64_to_cstr/scpp_uint64_to_cstr on the same
// thread (mirrors scpp_string_c_str's own "valid until next call"
// invalidation rule). Used to implement std::to_string(std::int64_t)/
// std::to_string(std::size_t) in std_string.scpp.
const char* scpp_int64_to_cstr(std::int64_t value);
const char* scpp_uint64_to_cstr(std::size_t value);

#ifdef __cplusplus
}
#endif

#endif // SCPP_STRING_WRAPPER_H
