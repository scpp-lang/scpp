// spec §6.4: an ordinary (non-move) reassignment of a class-typed
// variable is still rejected -- only `y = std::move(x);` (the
// compiler-synthesized move assignment operator) is licensed; a plain
// `y = x;` would be an unsupported bitwise copy (ch04 §4.2, unchanged
// by this feature).
class Widget {
public:
    Widget() { return; }
    int val;
};
int main() {
    Widget x;
    Widget y;
    y = x;
    return 0;
}
