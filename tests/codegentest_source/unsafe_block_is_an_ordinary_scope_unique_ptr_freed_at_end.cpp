int main() {
    unsafe {
        std::unique_ptr<int> a = std::make_unique<int>(1);
    }
    return 0;
}
