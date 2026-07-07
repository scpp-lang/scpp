// spec §6.5(3): symmetric to copy_construction_compiler_provided_is_allowed.cpp
// -- a plain class also has an implicitly-defined copy assignment
// operator under the same conditions. `b = a;` (bare reassignment) is
// licensed.
class Widget {
public:
    Widget() { return; }
    int val;
};
int main() {
    Widget a;
    Widget b;
    b = a;
    return b.val;
}
