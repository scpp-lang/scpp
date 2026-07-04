extern "C" int puts(const char* s);
safe void greet() {
    unsafe {
        puts("Hello, World!");
    }
    return;
}
int main() {
    greet();
    return 0;
}
