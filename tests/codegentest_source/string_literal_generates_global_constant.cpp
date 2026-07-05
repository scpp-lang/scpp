extern "C" int puts(const char* s);
void f() {
    unsafe {
        puts("hi");
    }
    return;
}
