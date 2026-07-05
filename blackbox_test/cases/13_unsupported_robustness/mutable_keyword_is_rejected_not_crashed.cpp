// ch04 §4.2/ch06/ch08 Q4 backlog: "mutable" (phase-1 interior mutability,
// reusing real C++'s keyword but stricter) is "design finalized ... not
// yet implemented" -- the keyword isn't recognized as a declaration
// specifier yet.
class Counter {
private:
    mutable int cache;
public:
    safe Counter() {
        this->cache = 0;
        return;
    }
    safe ~Counter() {
        return;
    }
    safe int get() const {
        this->cache = this->cache + 1;
        return this->cache;
    }
};

int main() {
    Counter c;
    return c.get();
}
