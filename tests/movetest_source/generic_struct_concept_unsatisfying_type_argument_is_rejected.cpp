// ch05 §5.14: a generic struct's own type parameter must be concept-
// constrained -- instantiating it with a concrete type argument that
// does *not* satisfy that concept is rejected immediately, with a
// precise diagnostic (unlike a generic class method's own
// per-member requires-clause, which is checked lazily per call --
// here there is only one, class-wide constraint, checked as soon as
// the instantiation itself is resolved).
template<typename T>
concept Describable = requires(const T& t) {
    { t.magnitude() } -> std::same_as<int>;
};

template<Describable T>
struct Wrapper {
    T item;
};

int main() {
    Wrapper<int> w;
    return 0;
}
