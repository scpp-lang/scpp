import std;
int f() {
    while (true) {
        std::unique_ptr<int> a = std::make_unique<int>();
        std::unique_ptr<int> b = std::move(a);
    }
    return 0;
}
