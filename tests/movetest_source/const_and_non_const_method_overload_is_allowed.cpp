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
    safe int get() {
        this->value = this->value + 1;
        return this->value;
    }
};
safe int use_const(const Counter& c) {
    return c.get();
}
safe int use_mutable(Counter& c) {
    return c.get();
}
