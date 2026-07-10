#pragma once

// Native helper API behind std:io's safe `std::print` / `std::println`
// surface. User scpp code never calls these directly; std_io.scpp hides the
// unsafe FFI boundary and exposes a checked wrapper instead.

extern "C" {

void* scpp_format_args_new();
void scpp_format_args_delete(void* handle);
void scpp_format_args_push_int(void* handle, int value);
void scpp_format_args_push_bool(void* handle, bool value);
void scpp_format_args_push_char(void* handle, char value);
void scpp_format_args_push_double(void* handle, double value);
void scpp_format_args_push_cstring(void* handle, const char* value);
void scpp_format_write(void* handle, const char* fmt, bool append_newline);
void scpp_format_write_empty_line();

}
