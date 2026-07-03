int main() {
    std::unique_ptr<int> a = std::make_unique<int>(42);
    std::unique_ptr<int> b = std::move(a);
    return 7;
}
