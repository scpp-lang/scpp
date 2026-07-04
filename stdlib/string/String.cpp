// String.cpp
//
// scpp's `class String`: a thin, safe wrapper around the extern "C"
// functions declared below, which are themselves a plain-C-ABI shim over
// real C++ `std::string` (see scpp_string_wrapper.h/.cpp, built separately
// as a native library -- stdlib/string/README.md explains the full build
// story). This is scpp's concrete demonstration of calling into a real
// C/C++ library: every method's body is `unsafe { <one extern "C" call> }`,
// exactly like any other native-function call from safe code (ch01 §1.3).
//
// This file has no `main()` -- it's a library source, meant to be built
// together with a consumer (see demo.cpp) since scpp v0.1 has no
// multi-file/include mechanism yet (a single `scpp build` invocation
// compiles exactly one source file). build.sh concatenates this file with
// demo.cpp before invoking `scpp build`.
extern "C" {
    void* scpp_string_new(const char* s);
    void scpp_string_delete(void* handle);
    int scpp_string_length(void* handle);
    const char* scpp_string_c_str(void* handle);
    void scpp_string_append(void* handle, const char* s);
    int scpp_string_equals(void* handle, const char* s);
}

class String {
private:
    // Opaque handle onto the real std::string heap-allocated on String's
    // behalf by scpp_string_new -- scpp itself never sees `std::string`,
    // only this untyped handle (ch04 §4.2: a member variable is never
    // public, so this representation detail can never leak to callers
    // even accidentally).
    void* handle;

public:
    safe String(const char* s) {
        unsafe {
            this->handle = scpp_string_new(s);
        }
        return;
    }

    safe ~String() {
        unsafe {
            scpp_string_delete(this->handle);
        }
        return;
    }

    // Number of bytes currently stored (std::string::size()).
    safe int length() const {
        unsafe {
            return scpp_string_length(this->handle);
        }
    }

    // The string's content as a nul-terminated C string, e.g. to hand to
    // another extern "C" function like puts(). Valid only until the next
    // call to append() on this same String (matches std::string::c_str()'s
    // own invalidation rule -- see scpp_string_wrapper.h).
    safe const char* c_str() const {
        unsafe {
            return scpp_string_c_str(this->handle);
        }
    }

    // Appends `s` to this String's content in place.
    safe void append(const char* s) {
        unsafe {
            scpp_string_append(this->handle, s);
        }
        return;
    }

    // True if this String's content equals `s` byte-for-byte.
    safe bool equals(const char* s) const {
        unsafe {
            return scpp_string_equals(this->handle, s) != 0;
        }
    }
};
