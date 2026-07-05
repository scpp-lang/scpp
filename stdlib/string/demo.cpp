// demo.cpp
//
// End-to-end demo of stdlib/string's std::string, consumed through
// scpp's real multi-file module system (ch11): imports the "std" module
// (see std.cpp) and constructs a std::string from a C string literal,
// appends to it, reads its length, converts it back to a C string to
// print via libc's real puts(), and checks equality. See CMakeLists.txt
// for how this file is compiled and linked against std.cpp's *own*,
// separately-compiled object file plus the compiled scpp_string_wrapper
// library.
import std;

extern "C" {
    int puts(const char* s);
}

safe void print_string(const std::string& s) {
    unsafe {
        puts(s.c_str());
    }
    return;
}

int main() {
    std::string greeting("Hello");
    greeting.append(", ");
    greeting.append("scpp");
    greeting.append("!");
    print_string(greeting);
    print_int(greeting.length());
    print_bool(greeting.equals("Hello, scpp!"));
    print_bool(greeting.equals("something else"));
    return 0;
}
