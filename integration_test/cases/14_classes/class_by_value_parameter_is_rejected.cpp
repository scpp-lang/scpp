// stdlib/string's README and ch04 §4.2: "No copy semantics ... movecheck
// rejects [by-value parameters] ... this isn't specific to String; it
// applies to every class type in this version, since scpp has no
// copy-constructor concept yet." Pass `const Counter&`/`Counter&`
// instead.
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

safe void take_by_value(Counter c) {
    return;
}

int main() {
    Counter c(5);
    take_by_value(c);
    return 0;
}
