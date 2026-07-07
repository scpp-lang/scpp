// scpp_string_wrapper.h
//
// A plain C ABI (`extern "C"`) wrapper around real C++ `std::string`,
// consumed by stdlib/string/String.cpp's scpp `class String`. This is the
// concrete demonstration of scpp calling into a real C/C++ library (see
// stdlib/README.md): the wrapper is compiled by an ordinary C++
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

#ifdef __cplusplus
extern "C" {
#endif

// Allocates a new std::string, copy-initialized from the given
// nul-terminated C string (an empty string if `s` is NULL). Returns an
// opaque handle; ownership transfers to the caller, who must eventually
// pass it to scpp_string_delete exactly once.
void* scpp_string_new(const char* s);

// Destroys a handle previously returned by scpp_string_new. `handle` must
// not be used again afterward (matches std::string's own destructor
// semantics -- this *is* that destructor, just called through a plain
// function instead of C++'s implicit mechanism).
void scpp_string_delete(void* handle);

// Returns the string's length in bytes (std::string::size()).
int scpp_string_length(void* handle);

// Returns a pointer to the string's internal nul-terminated buffer
// (std::string::c_str()). Valid only until the next mutating call
// (scpp_string_append) or scpp_string_delete on the same handle -- same
// invalidation rule as real std::string::c_str().
const char* scpp_string_c_str(void* handle);

// Appends a nul-terminated C string to `handle` in place (a no-op if `s`
// is NULL).
void scpp_string_append(void* handle, const char* s);

// Returns 1 if `handle`'s content equals the nul-terminated C string `s`,
// 0 otherwise (including when `s` is NULL).
int scpp_string_equals(void* handle, const char* s);

#ifdef __cplusplus
}
#endif

#endif // SCPP_STRING_WRAPPER_H
