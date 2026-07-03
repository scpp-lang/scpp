int main() {
    int i = 0;
    while (i < 10) {
        std::unique_ptr<int> a = std::make_unique<int>(i);
        i = i + 1;
    }
    return 0;
}
