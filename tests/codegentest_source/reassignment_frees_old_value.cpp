int main() {
    std::unique_ptr<int> a = std::make_unique<int>(1);
    a = std::make_unique<int>(2);
    return 0;
}
