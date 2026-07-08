// spec §6.5(2): a plain class with no user-declared destructor, copy
// constructor, or copy assignment operator (and every field itself
// copy-constructible -- here, a plain scalar) has an implicitly-defined
// copy constructor. `Widget b = a;` (the bare copy-init syntax) is
// licensed.
class Widget {
public:
    Widget() { return; }
    int val;
};
int main() {
    Widget a;
    Widget b = a;
    return b.val;
}
