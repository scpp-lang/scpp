// Same rule as the read case, but writing to a private field from
// outside the class.
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
    c.value = 9;
    return 0;
}
