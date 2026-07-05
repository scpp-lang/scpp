// Counterpart: wrapping the non-`safe` method call in `unsafe { }` makes
// it legal, exactly like calling any other non-`safe` function.
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
    unsafe {
        return c.get_native();
    }
}

int main() {
    return f();
}
