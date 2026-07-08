int main() {
    int x = 5;
    auto f = [&x]() { x = 10; return x; };
    int y = f();
    print_int(x + y);
    return 0;
}
