#include "scpp_unordered_set_wrapper.h"

#include <string>
#include <unordered_set>

namespace {

using string_set = std::unordered_set<std::string>;

}

extern "C" {

void* scpp_unordered_set_string_new() {
    return new string_set();
}

void* scpp_unordered_set_string_copy(const void* handle) {
    return new string_set(*static_cast<const string_set*>(handle));
}

void scpp_unordered_set_string_delete(void* handle) {
    delete static_cast<string_set*>(handle);
}

int scpp_unordered_set_string_insert(void* handle, const char* value) {
    return static_cast<string_set*>(handle)->insert(std::string(value)).second ? 1 : 0;
}

int scpp_unordered_set_string_contains(const void* handle, const char* value) {
    return static_cast<const string_set*>(handle)->contains(value) ? 1 : 0;
}

int scpp_unordered_set_string_erase(void* handle, const char* value) {
    return static_cast<string_set*>(handle)->erase(value) != 0 ? 1 : 0;
}

int scpp_unordered_set_string_size(const void* handle) {
    return static_cast<int>(static_cast<const string_set*>(handle)->size());
}

int scpp_unordered_set_string_empty(const void* handle) {
    return static_cast<const string_set*>(handle)->empty() ? 1 : 0;
}

void scpp_unordered_set_string_clear(void* handle) {
    static_cast<string_set*>(handle)->clear();
}

}
