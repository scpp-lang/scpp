// scpp_charconv_wrapper.cpp
//
// Thin native glue for std::from_chars so the SCPP stdlib can expose the
// standard C++ API shape while keeping all raw-pointer/extern interactions
// inside library-owned unsafe blocks.
#include <charconv>
#include <system_error>

extern "C" {

const char* scpp_std_from_chars_int(const char* first, const char* last, int* out_value, int* out_ec, int base) {
    std::from_chars_result result = std::from_chars(first, last, *out_value, base);
    *out_ec = static_cast<int>(result.ec);
    return result.ptr;
}

} // extern "C"
