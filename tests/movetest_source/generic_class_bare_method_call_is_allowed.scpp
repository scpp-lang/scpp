// ch05 §5.14: a generic class's own type parameter may be left bare
// ("no operations guaranteed beyond the universal move/store/pass-
// through/return baseline") -- calling a method with no `requires`
// clause is always allowed, regardless of the concrete type argument.
template<typename T>
class Box {
    T value;
public:
    Box(const T& v) { this.value = v; return; }
    const T& get() const { return this.value; }
};

int main() {
    int n = 5;
    Box<int> b(n);
    return b.get();
}
