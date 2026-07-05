// Same no-copy-semantics rule, for a return type instead of a parameter.
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

safe Counter make_counter() {
    Counter c(5);
    return c;
}

int main() {
    return 0;
}
