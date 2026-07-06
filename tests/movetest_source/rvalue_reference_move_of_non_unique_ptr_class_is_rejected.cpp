// ch03/ch05 §5.11 v1 scoping: std::move is only supported for
// std::unique_ptr variables in this version (unchanged, pre-existing
// restriction) -- a class-typed local cannot be moved into a `T&&`
// parameter this way, even though the *parameter* itself is otherwise a
// perfectly legal rvalue-reference to a class type.
class Widget {
public:
    Widget() {}
    int get() const { return 1; }
};
int take(Widget&& w) { return w.get(); }
int main() {
    Widget w;
    return take(std::move(w));
}
