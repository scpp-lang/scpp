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
safe int f() {
    Counter c(5);
    return c.get();
}
