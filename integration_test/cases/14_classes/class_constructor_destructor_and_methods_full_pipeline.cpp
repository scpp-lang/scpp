// ch04 §4.2 / ch05 §5.9: a `class` with a constructor, destructor, and
// methods -- the basic end-to-end shape. Constructors/destructors follow
// real C++ syntax (`ClassName(...)`/`~ClassName()`); private state is
// only reachable through methods.
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

// Counter(5), then increment() twice -> 7.
safe int f() {
    Counter c(5);
    c.increment();
    c.increment();
    return c.get();
}

int main() {
    return f();
}
