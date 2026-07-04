// ch02 §2.1: variadic parameters ("...", for printf-family functions) are
// parsed and carry a has_varargs flag; this exercises an actual variadic
// call site with a real `int` argument, not just the fixed `fmt`
// parameter. `main` is a native function, so no `unsafe { }` is needed to
// call the non-`safe` `printf`.
extern "C" int printf(const char* fmt, ...);

int main() {
    int x = 99;
    printf("%d\n", x);
    return 0;
}
