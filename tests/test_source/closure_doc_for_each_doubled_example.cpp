// ch05 §5.11/§5.12: the doc's own motivating example for closures +
// concepts together -- a closure passed to a `Concept auto&&` generic
// parameter, invoked via the bare `f(x)` call syntax (not `.call(x)`)
// inside the generic function's own body.
template<typename T>
concept IntConsumer = requires(T f, int x) { f(x); };

int print_doubled(std::span<int> s, IntConsumer auto&& f) {
    int i = 0;
    while (i < s.size) {
        f(s[i] * 2);
        i = i + 1;
    }
    return 0;
}

int main() {
    int arr[3];
    arr[0] = 1;
    arr[1] = 2;
    arr[2] = 3;
    std::span<int> s = arr;
    print_doubled(s, [](int x) { print_int(x); });
    return 0;
}
