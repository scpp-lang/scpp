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
};
Counter make_counter() {
    Counter c(5);
    return c;
}
