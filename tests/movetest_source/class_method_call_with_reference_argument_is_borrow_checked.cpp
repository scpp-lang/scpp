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
safe int f() {
    Adder a(5);
    int y = 3;
    a.add_ref(y);
    return a.get();
}
