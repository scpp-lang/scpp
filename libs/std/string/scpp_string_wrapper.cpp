// scpp_string_wrapper.cpp
//
// Implementation of the extern "C" wrapper declared in
// scpp_string_wrapper.h. Every function is a thin, direct forward onto
// real std::string -- this file's only job is translating between the
// plain-C ABI scpp can call and std::string's actual C++ API; see that
// header for the exported contract and libs/README.md for how
// this fits into the overall String demo.
#include "scpp_string_wrapper.h"

#include <string>

namespace {

std::string* as_string(void* handle) { return static_cast<std::string*>(handle); }

} // namespace

extern "C" {

void* scpp_string_new(const char* s) { return new std::string(s != nullptr ? s : ""); }

void scpp_string_delete(void* handle) { delete as_string(handle); }

int scpp_string_length(void* handle) { return static_cast<int>(as_string(handle)->size()); }

const char* scpp_string_c_str(void* handle) { return as_string(handle)->c_str(); }

const char* scpp_cstr_end(const char* s) {
    if (s == nullptr) return nullptr;
    const char* current = s;
    while (*current != '\0') current++;
    return current;
}

void scpp_string_append(void* handle, const char* s) {
    if (s != nullptr) as_string(handle)->append(s);
}

int scpp_string_equals(void* handle, const char* s) { return s != nullptr && *as_string(handle) == s ? 1 : 0; }

} // extern "C"
