// ch05 §5.12: a by-value (non-init) capture of a std::unique_ptr is an
// implicit copy of a move-only type -- rejected exactly like reading
// any other std::unique_ptr variable without std::move (ch05 §5.1).
// Use an init-capture instead: `[p = std::move(p)]`.
template<typename T>
concept IntTransform = requires(T f, int x) { f(x); };

int apply(IntTransform auto&& f, int z) {
    return f(z);
}

int main() {
    std::unique_ptr<int> p = std::make_unique<int>(5);
    return apply([p](int z) -> int { return z; }, 3);
}
