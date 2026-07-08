import std;

class MoveOnlyAdder {
private:
    std::unique_ptr<int> value;
public:
    MoveOnlyAdder(std::unique_ptr<int> value) {
        this->value = std::move(value);
        return;
    }

    int call(int x) {
        return x + *this->value;
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

class Inspect {
private:
    int value;
public:
    Inspect(int value) {
        this->value = value;
        return;
    }

    int call() const {
        return this->value;
    }
};

class InspectLvalue {
private:
    int value;
public:
    InspectLvalue(int value) {
        this->value = value;
        return;
    }

    int call() const & {
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

class InspectRvalue {
private:
    int value;
public:
    InspectRvalue(int value) {
        this->value = value;
        return;
    }

    int call() const && {
        return this->value + 2;
    }
};


int main() {
    std::move_only_function<int(int)> moved(MoveOnlyAdder(std::make_unique<int>(5)));
    std::move_only_function<int() &> counter(Counter(3));
    std::move_only_function<int() const> inspect(Inspect(11));
    std::move_only_function<int() const &> inspect_lvalue(InspectLvalue(13));
    std::move_only_function<int() &&> consume(Consume(9));
    std::move_only_function<int() const &&> inspect_rvalue(InspectRvalue(20));
    return moved(7) + counter() + inspect() + inspect_lvalue() + std::move(consume)() + std::move(inspect_rvalue)();
}
