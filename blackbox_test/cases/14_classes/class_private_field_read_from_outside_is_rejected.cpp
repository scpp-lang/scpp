// ch04 §4.2: "a member variable ... can never be public; only member
// functions can be. ... External code can therefore only ever reach a
// class's data through a method call, never through direct field
// access." Reading a private field from outside the class is rejected.
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
    Counter c(5);
    return c.value;
}
