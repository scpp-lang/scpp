class Callable {
public:
    int call() & { return 1; }
    int call() && { return 2; }
};

int main() {
    Callable c;
    print_int(c());
    print_int(std::move(c)());
    return 0;
}
