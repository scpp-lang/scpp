class Counter {
private:
    int value;
public:
    Counter(int start) {
        this->value = start;
        return;
    }
    Counter(const Counter& other) {
        print_int(111);
        this->value = other.value;
        return;
    }
    int get() {
        return this->value;
    }
};
int take_by_value(Counter c) {
    return c.get();
}
int main() {
    Counter counter(7);
    print_int(take_by_value(counter));
    return 0;
}
