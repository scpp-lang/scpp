// ch05 §5.12: mixed capture forms -- `[&, x]` (blanket by-reference,
// 'x' explicitly overridden to by-value) and `[=, &y]` (blanket
// by-value, 'y' explicitly overridden to by-reference).
template<typename T>
concept IntTransform = requires(T f, int x) { f(x); };

int apply(IntTransform auto&& f, int z) {
    return f(z);
}

int main() {
    int a = 5;
    int b = 10;
    print_int(apply([&, a](int z) -> int { return a + b + z; }, 3));
    print_int(apply([=, &b](int z) -> int { return a + b + z; }, 3));
    return 0;
}
