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
    safe void increment() {
        this->value = this->value + 1;
        return;
    }
};
int main() {
    Counter c(5);
    c.increment();
    c.increment();
    print_int(c.get());
    return 0;
}
