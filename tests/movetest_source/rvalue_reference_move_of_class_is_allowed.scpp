import std;
// spec §6.4/ch04 §4.2: now that class types have move construction and
// move assignment (the compiler-synthesized memberwise operation), a
// class-typed local can be moved into a `T&&` parameter exactly like
// std::unique_ptr always could -- `std::move(w)` marks `w` moved-out and
// hands `take` a fresh rvalue of the same class type.
class Widget {
public:
    Widget() { return; }
    int get() const { return 1; }
};
int take(Widget&& w) { return w.get(); }
int main() {
    Widget w;
    return take(std::move(w));
}
