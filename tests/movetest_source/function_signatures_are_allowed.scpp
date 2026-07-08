import std;

class Adder {
private:
    int value;
public:
    Adder(int value) {
        this->value = value;
        return;
    }

    int call(int x) const {
        return x + this->value;
    }
};

class Counter {
private:
    int value;
public:
    Counter(int value) {
        this->value = value;
        return;
    }

    int call() & {
        this->value = this->value + 1;
        return this->value;
    }
};

class Consume {
private:
    int value;
public:
    Consume(int value) {
        this->value = value;
        return;
    }

    int call() && {
        return this->value + 1;
    }
};

int main() {
    std::function<int(int) const> add(Adder(5));
    std::function<int() &> counter(Counter(3));
    std::function<int() &&> consume(Consume(9));
    std::function<int(int) const> copied = add;
    return copied(7) + counter() + std::move(consume)();
}
