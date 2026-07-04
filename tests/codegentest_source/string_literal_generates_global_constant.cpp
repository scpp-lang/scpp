extern "C" int puts(const char* s);
safe void f() {
    unsafe {
        puts("hi");
    }
    return;
}
