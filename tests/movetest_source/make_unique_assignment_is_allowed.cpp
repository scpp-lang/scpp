import std;
int f() {
    std::unique_ptr<int> a;
    a = std::make_unique<int>(5);
    return 0;
}
