#include <cstdio>

extern "C" {

void scpp_print_int(int value) { std::printf("%d", value); }

void scpp_print_bool(bool value) { std::fputs(value ? "true" : "false", stdout); }

void scpp_print_char(char value) { std::putchar(value); }

void scpp_print_double(double value) { std::printf("%g", value); }

void scpp_print_cstr(const char* value) { std::fputs(value != nullptr ? value : "(null)", stdout); }

void scpp_print_newline() { std::putchar('\n'); }

} // extern "C"
