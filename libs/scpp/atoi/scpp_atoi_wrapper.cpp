#include <charconv>
#include <cstdint>
#include <cstring>
#include <system_error>

extern "C" int scpp_atoi_parse_i32(const char* text, std::int32_t* value) {
    if (text == nullptr || value == nullptr) return 2;
    if (text[0] == '\0') return 1;

    const char* end = text + std::strlen(text);
    std::from_chars_result result = std::from_chars(text, end, *value);
    if (result.ec == std::errc()) {
        return result.ptr == end ? 0 : 2;
    }
    if (result.ec == std::errc::result_out_of_range) return 3;
    return 2;
}
