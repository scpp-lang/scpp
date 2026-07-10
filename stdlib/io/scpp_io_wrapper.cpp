#include "scpp_io_wrapper.h"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using FormatArg = std::variant<int, bool, char, double, std::string>;

struct FormatArgList {
    std::vector<FormatArg> values;
};

[[noreturn]] void fail_format(std::string_view message) {
    std::cerr << "std::print/std::println format error: " << message << '\n';
    std::exit(1);
}

std::string render_arg(const FormatArg& arg) {
    return std::visit(
        [](const auto& value) -> std::string {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, bool>) {
                return value ? "true" : "false";
            } else if constexpr (std::is_same_v<T, char>) {
                return std::string(1, value);
            } else if constexpr (std::is_same_v<T, std::string>) {
                return value;
            } else {
                std::ostringstream out;
                out << value;
                return out.str();
            }
        },
        arg);
}

std::string format_text(const FormatArgList& args, std::string_view fmt) {
    std::string out;
    size_t arg_index = 0;
    for (size_t i = 0; i < fmt.size(); i++) {
        char ch = fmt[i];
        if (ch == '{') {
            if (i + 1 >= fmt.size()) fail_format("unterminated '{' in format string");
            if (fmt[i + 1] == '{') {
                out.push_back('{');
                i++;
                continue;
            }
            if (fmt[i + 1] != '}') {
                fail_format("only bare '{}' placeholders are supported in v1");
            }
            if (arg_index >= args.values.size()) {
                fail_format("placeholder count exceeds provided argument count");
            }
            out += render_arg(args.values[arg_index]);
            arg_index++;
            i++;
            continue;
        }
        if (ch == '}') {
            if (i + 1 < fmt.size() && fmt[i + 1] == '}') {
                out.push_back('}');
                i++;
                continue;
            }
            fail_format("unmatched '}' in format string");
        }
        out.push_back(ch);
    }
    if (arg_index != args.values.size()) {
        fail_format("provided argument count exceeds placeholder count");
    }
    return out;
}

FormatArgList& require_args(void* handle) {
    if (handle == nullptr) fail_format("internal null format-argument handle");
    return *static_cast<FormatArgList*>(handle);
}

} // namespace

extern "C" {

void* scpp_format_args_new() { return new FormatArgList(); }

void scpp_format_args_delete(void* handle) { delete static_cast<FormatArgList*>(handle); }

void scpp_format_args_push_int(void* handle, int value) { require_args(handle).values.emplace_back(value); }

void scpp_format_args_push_bool(void* handle, bool value) { require_args(handle).values.emplace_back(value); }

void scpp_format_args_push_char(void* handle, char value) { require_args(handle).values.emplace_back(value); }

void scpp_format_args_push_double(void* handle, double value) { require_args(handle).values.emplace_back(value); }

void scpp_format_args_push_cstring(void* handle, const char* value) {
    require_args(handle).values.emplace_back(value == nullptr ? std::string("(null)") : std::string(value));
}

void scpp_format_write(void* handle, const char* fmt, bool append_newline) {
    FormatArgList& args = require_args(handle);
    std::string out = format_text(args, fmt == nullptr ? std::string_view() : std::string_view(fmt));
    if (append_newline) {
        std::cout << out << '\n';
    } else {
        std::cout << out;
    }
    std::cout.flush();
}

void scpp_format_write_empty_line() {
    std::cout << '\n';
    std::cout.flush();
}

}
