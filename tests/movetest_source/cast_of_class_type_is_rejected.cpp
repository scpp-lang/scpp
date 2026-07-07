// ch06 §6: a cast is only supported between two scalar types -- casting
// a class instance is rejected (codegen has no meaningful conversion
// instruction to emit for it).
class Widget {
public:
    Widget() { return; }
    int val;
};
int main() {
    Widget w;
    int x = (int)w;
    return x;
}
