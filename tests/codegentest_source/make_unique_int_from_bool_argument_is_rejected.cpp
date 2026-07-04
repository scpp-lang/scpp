int main() {
    std::unique_ptr<int> p = std::make_unique<int>(true);
    return *p;
}
