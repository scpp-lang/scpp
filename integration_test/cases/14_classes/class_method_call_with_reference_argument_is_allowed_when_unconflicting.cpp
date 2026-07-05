// Counterpart: the same call is fine once `y` isn't concurrently borrowed
// by anything else.
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
    safe int get() const {
        return this->value;
    }
};

// Adder(5), add_ref(3) -> value becomes 8.
safe int f() {
    Adder a(5);
    int y = 3;
    a.add_ref(y);
    return a.get();
}

int main() {
    return f();
}
