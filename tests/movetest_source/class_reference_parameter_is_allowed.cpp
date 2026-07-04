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
    safe int get() const {
        return this->value;
    }
};
safe int take_by_reference(const Counter& c) {
    return c.get();
}
safe int f() {
    Counter a(5);
    return take_by_reference(a);
}
