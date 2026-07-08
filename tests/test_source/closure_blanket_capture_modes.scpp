// ch05 §5.12: `[=]` (blanket by-value) and `[&]` (blanket by-reference)
// both correctly resolve every free variable referenced in the body via
// free-variable analysis, with no explicit capture list at all.
template<typename T>
concept IntTransform = requires(T f, int x) { f(x); };

int apply(IntTransform auto&& f, int z) {
    return f(z);
}

int main() {
    int a = 5;
    int b = 10;
    print_int(apply([=](int z) -> int { return a + b + z; }, 3));
    print_int(apply([&](int z) -> int { return a + b + z; }, 3));
    return 0;
}
