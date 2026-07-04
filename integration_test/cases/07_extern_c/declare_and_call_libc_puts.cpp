// ch02 §2.1: a single `extern "C"` declaration for a real libc function.
// `main` here is an ordinary native function (ch01: unannotated -> a
// native function, already unsafe everywhere), so calling the non-`safe`
// `puts` needs no `unsafe { }` wrapper at all -- that's only required
// when the *caller* is itself `safe` (ch02's boundary table).
extern "C" int puts(const char* s);

int main() {
    puts("scpp integration test");
    return 0;
}
