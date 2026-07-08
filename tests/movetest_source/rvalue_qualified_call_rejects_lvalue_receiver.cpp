class Callable {
public:
    int call() && { return 1; }
};

int main() {
    Callable c;
    return c();
}
