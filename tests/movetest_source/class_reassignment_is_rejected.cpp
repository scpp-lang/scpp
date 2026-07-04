class Counter {
private:
    int value;
public:
    safe Counter(int start) {
        this->value = start;
        return;
    }
    safe ~Counter() {
        return;
    }
};
safe void f() {
    Counter a(5);
    Counter b(10);
    a = b;
    return;
}
