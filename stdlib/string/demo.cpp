// demo.cpp
//
// End-to-end demo of stdlib/string's `class String`: constructs a String
// from a C string literal, appends to it, reads its length, converts it
// back to a C string to print via libc's real puts(), and checks equality.
// See build.sh for how this file gets combined with String.cpp and linked
// against the compiled scpp_string_wrapper library into one executable.
extern "C" {
    int puts(const char* s);
}

safe void print_string(const String& s) {
    unsafe {
        puts(s.c_str());
    }
    return;
}

int main() {
    String greeting("Hello");
    greeting.append(", ");
    greeting.append("scpp");
    greeting.append("!");
    print_string(greeting);
    print_int(greeting.length());
    print_bool(greeting.equals("Hello, scpp!"));
    print_bool(greeting.equals("something else"));
    return 0;
}
