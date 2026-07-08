// ch05 §5.12: explicit by-value and by-reference captures both work
// together, with the closure passed via a `Concept auto&&` parameter
// and invoked via the bare `f(x)` call syntax.
template<typename T>
concept IntTransform = requires(T f, int x) { f(x); };

int apply(IntTransform auto&& f, int z) {
    return f(z);
}

int main() {
    int a = 5;
    int b = 10;
    print_int(apply([a, &b](int z) -> int { return a + b + z; }, 3));
    return 0;
}
