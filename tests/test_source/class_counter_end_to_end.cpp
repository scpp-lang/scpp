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
    void increment() {
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
