// Same no-copy-semantics rule, for plain reassignment of an existing
// class-typed variable.
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

int main() {
    Counter a(5);
    Counter b(9);
    a = b;
    return 0;
}
