import std;
int consume(std::unique_ptr<int> p) {
    return 0;
}

int f() {
    std::unique_ptr<int> a;
    return consume(std::move(a));
}
