// scpp_string_wrapper.cpp
//
// Implementation of the extern "C" wrapper declared in
// scpp_string_wrapper.h. Every function is a thin, direct forward onto
// real std::string -- this file's only job is translating between the
// plain-C ABI scpp can call and std::string's actual C++ API; see that
// header for the exported contract and libs/README.md for how
// this fits into the overall String demo.
#include "scpp_string_wrapper.h"

#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

std::string* as_string(void* handle) { return static_cast<std::string*>(handle); }
const std::string* as_string(const void* handle) { return static_cast<const std::string*>(handle); }

} // namespace

extern "C" {

void* scpp_string_new(const char* s) { return new std::string(s != nullptr ? s : ""); }

void* scpp_string_new_sized(const char* s, std::size_t n) {
    if (s == nullptr) return new std::string();
    return new std::string(s, n);
}

void* scpp_string_copy(const void* handle) { return new std::string(*as_string(handle)); }

void scpp_string_delete(void* handle) { delete as_string(handle); }

std::size_t scpp_string_length(void* handle) { return as_string(handle)->size(); }

void scpp_string_reserve(void* handle, std::size_t n) { as_string(handle)->reserve(n); }

const char* scpp_string_c_str(void* handle) { return as_string(handle)->c_str(); }

void scpp_string_append(void* handle, const char* s) {
    if (s != nullptr) as_string(handle)->append(s);
}

void scpp_string_push_back(void* handle, char c) { as_string(handle)->push_back(c); }

int scpp_string_equals(void* handle, const char* s) { return s != nullptr && *as_string(handle) == s ? 1 : 0; }

int scpp_string_contains(void* handle, const char* needle) {
    return needle != nullptr && as_string(handle)->find(needle) != std::string::npos ? 1 : 0;
}

std::size_t scpp_string_find(void* handle, const char* needle, std::size_t start) {
    if (needle == nullptr) return std::string::npos;
    return as_string(handle)->find(needle, start);
}

std::size_t scpp_string_find_char(void* handle, char needle, std::size_t start) {
    return as_string(handle)->find(needle, start);
}

std::size_t scpp_string_rfind(void* handle, const char* needle) {
    if (needle == nullptr) return std::string::npos;
    return as_string(handle)->rfind(needle);
}

std::size_t scpp_string_rfind_char(void* handle, char needle) { return as_string(handle)->rfind(needle); }

int scpp_string_ends_with(void* handle, const char* suffix) {
    if (suffix == nullptr) return 0;
    const std::string* s = as_string(handle);
    std::string_view suffix_view(suffix);
    return s->size() >= suffix_view.size() && s->compare(s->size() - suffix_view.size(), suffix_view.size(), suffix_view) == 0
               ? 1
               : 0;
}

const char* scpp_string_substr(void* handle, std::size_t pos, std::size_t len) {
    const std::string* s = as_string(handle);
    if (pos > s->size()) std::abort();
    thread_local std::string buf;
    buf = s->substr(pos, len);
    return buf.c_str();
}


const char* scpp_int64_to_cstr(std::int64_t value) {
    thread_local std::string buf;
    buf = std::to_string(value);
    return buf.c_str();
}

const char* scpp_uint64_to_cstr(std::size_t value) {
    thread_local std::string buf;
    buf = std::to_string(value);
    return buf.c_str();
}

} // extern "C"
