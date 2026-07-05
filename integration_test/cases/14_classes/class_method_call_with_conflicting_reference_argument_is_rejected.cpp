// ch05 §5.9: "Calling a method borrows the receiver, exactly like passing
// a reference argument to an ordinary function" -- and an ordinary
// reference-typed method *parameter* is borrow-checked exactly like any
// other reference parameter. Here `y` is already mutably borrowed by `r`
// (still live -- used in the final `return`), so passing it by mutable
// reference into `add_ref` must conflict.
class Adder {
private:
    int value;
public:
    safe Adder(int start) {
        this->value = start;
        return;
    }
    safe ~Adder() {
        return;
    }
    safe void add_ref(int& x) {
        this->value = this->value + x;
        return;
    }
};

safe int f() {
    Adder a(5);
    int y = 3;
    int& r = y;
    a.add_ref(y);
    return r;
}

int main() {
    return f();
}
