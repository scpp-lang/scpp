// ch02 §2.1: the `extern "C" { ... }` block form is sugar for repeating
// `extern "C"` on every declaration inside it. `main` is a native
// function, so no `unsafe { }` is needed here.
extern "C" {
    int puts(const char* s);
    void abort();
}

int main() {
    puts("block form works");
    return 0;
}
