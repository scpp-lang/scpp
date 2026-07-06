// ch05 §5.12: an init-capture (`[q = std::move(p)]`) is how a move-only
// type (std::unique_ptr) crosses into a closure -- the construction
// itself (moving `p` into the closure's own field) works end-to-end.
// (Dereferencing the captured unique_ptr field afterward hits a
// separate, pre-existing, closures-unrelated limitation -- assigning/
// dereferencing through *any* class's own std::unique_ptr-typed field
// isn't supported yet, verified against an ordinary hand-written class
// -- so this test deliberately doesn't exercise that.)
template<typename T>
concept IntTransform = requires(T f, int x) { f(x); };

int apply(IntTransform auto&& f, int z) {
    return f(z);
}

int main() {
    std::unique_ptr<int> p = std::make_unique<int>(100);
    print_int(apply([q = std::move(p)](int z) -> int { return z; }, 3));
    return 0;
}
