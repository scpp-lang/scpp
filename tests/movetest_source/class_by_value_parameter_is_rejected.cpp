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
void take_by_value(Counter c) {
    return;
}
