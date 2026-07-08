// spec §6.4(3)/(5): `y = std::move(x);` -- the compiler-synthesized move
// assignment operator -- works for an ordinary class, marking `x`
// moved-out.
class Widget {
public:
    Widget() { return; }
    int val;
};
int main() {
    Widget x;
    x.val = 5;
    Widget y;
    y = std::move(x);
    return y.val;
}
