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
    int get_native() const {
        return this->value;
    }
};
safe int f() {
    Counter c(5);
    return c.get_native();
}
