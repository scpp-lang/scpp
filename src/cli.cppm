module;

#include <iostream>
#include <string_view>

export module scpp.cli;

export namespace scpp {

constexpr std::string_view version = "0.1.0";

int run(int argc, char** argv) {
    std::string_view name = argc > 0 ? argv[0] : "scpp";
    std::cout << "Hello from " << name << " " << version << "!\n";
    return 0;
}

} // namespace scpp
