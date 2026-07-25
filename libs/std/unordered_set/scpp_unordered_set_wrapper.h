#pragma once

extern "C" {
void* scpp_unordered_set_string_new();
void* scpp_unordered_set_string_copy(const void* handle);
void scpp_unordered_set_string_delete(void* handle);
int scpp_unordered_set_string_insert(void* handle, const char* value);
int scpp_unordered_set_string_contains(const void* handle, const char* value);
int scpp_unordered_set_string_erase(void* handle, const char* value);
int scpp_unordered_set_string_size(const void* handle);
int scpp_unordered_set_string_empty(const void* handle);
void scpp_unordered_set_string_clear(void* handle);
}
