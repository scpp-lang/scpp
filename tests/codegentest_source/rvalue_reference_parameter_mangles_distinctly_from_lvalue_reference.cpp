// ch03: `T&&` (rvalue reference/move-parameter) is ABI-identical to
// `T&`/`const T&` at the LLVM level (a pointer, auto-dereferenced) --
// but must still mangle *distinctly* from an ordinary reference overload
// (ch05 §5.10/§11.9's existing per-parameter-type mangling scheme),
// otherwise two overloads differing only in `T&` vs `T&&` would collide
// on the same LLVM symbol name.
int f(int& x) { return x + 100; }
int f(int&& x) { return x + 200; }
int main() {
    int y = 5;
    return f(y) + f(10);
}
