import std;

extern "C" {
    int puts(const char* s);
}

void print_string(const std::string& s) {
    [[scpp::unsafe]] {
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
