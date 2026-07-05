// std.cpp
//
// scpp's "std" module: `namespace std { export class string { ... }; }`,
// a thin wrapper around the extern "C" functions declared below, which
// are themselves a plain-C-ABI shim over real C++ `std::string` (see
// scpp_string_wrapper.h/.cpp, built separately as a native library --
// stdlib/string/README.md explains the full build story). This is
// scpp's concrete demonstration of both calling into a real C/C++
// library and scpp's own multi-file module system (ch11
// docs/book/en/ch11-modules-and-libraries.md): every method's body is
// `unsafe { <one extern "C" call> }`, exactly like any other call to an
// `extern "C"` function (ch01 §1.3/ch02); `std::string` itself is
// exported from a real, separately-compiled module -- a consumer writes
// `import std;` then uses `std::string` (see demo.cpp), not a textual
// concatenation.
export module std;

// The extern "C" wrapper functions stay at file scope, *outside*
// `namespace std` -- they're plain C symbols (real C linkage, never
// namespace-qualified or mangled regardless of enclosing namespace, ch02
// §2.1/ch11 §11.9) and are not `export`-marked, so they're entirely
// invisible to anything that `import`s this module: only `std::string`
// itself is part of this module's public surface.
extern "C" {
    void* scpp_string_new(const char* s);
    void scpp_string_delete(void* handle);
    int scpp_string_length(void* handle);
    const char* scpp_string_c_str(void* handle);
    void scpp_string_append(void* handle, const char* s);
    int scpp_string_equals(void* handle, const char* s);
}

namespace std {

export class string {
private:
    // Opaque handle onto the real std::string heap-allocated on
    // string's behalf by scpp_string_new -- scpp itself never sees
    // `std::string`, only this untyped handle (ch04 §4.2: a member
    // variable is never public, so this representation detail can never
    // leak to callers even accidentally).
    void* handle;

public:
    string(const char* s) {
        unsafe {
            this->handle = scpp_string_new(s);
        }
        return;
    }

    ~string() {
        unsafe {
            scpp_string_delete(this->handle);
        }
        return;
    }

    // Number of bytes currently stored (std::string::size()).
    int length() const {
        unsafe {
            return scpp_string_length(this->handle);
        }
    }

    // The string's content as a nul-terminated C string, e.g. to hand to
    // another extern "C" function like puts(). Valid only until the next
    // call to append() on this same string (matches
    // std::string::c_str()'s own invalidation rule -- see
    // scpp_string_wrapper.h).
    const char* c_str() const {
        unsafe {
            return scpp_string_c_str(this->handle);
        }
    }

    // Appends `s` to this string's content in place.
    void append(const char* s) {
        unsafe {
            scpp_string_append(this->handle, s);
        }
        return;
    }

    // True if this string's content equals `s` byte-for-byte.
    bool equals(const char* s) const {
        unsafe {
            return scpp_string_equals(this->handle, s) != 0;
        }
    }
};

} // namespace std
