int main() {
    std::unique_ptr<int> p = std::make_unique<int>(10);
    int x = *p;
    return x;
}
