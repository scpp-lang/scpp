extern "C" int puts(const char* s);
void greet() {
    unsafe {
        puts("Hello, World!");
    }
    return;
}
int main() {
    greet();
    return 0;
}
