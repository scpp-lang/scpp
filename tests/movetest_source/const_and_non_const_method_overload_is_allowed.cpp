class Counter {
private:
    int value;
public:
    Counter(int start) {
        this->value = start;
        return;
    }
    ~Counter() {
        return;
    }
    int get() const {
        return this->value;
    }
    int get() {
        this->value = this->value + 1;
        return this->value;
    }
};
int use_const(const Counter& c) {
    return c.get();
}
int use_mutable(Counter& c) {
    return c.get();
}
