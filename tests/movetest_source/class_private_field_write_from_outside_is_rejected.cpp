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
    Counter c(5);
    c.value = 99;
    return;
}
