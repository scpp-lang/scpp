extern "C" int puts(const char* s);
void f() {
    [[scpp::unsafe]] {
        puts("hi");
    }
    return;
}
