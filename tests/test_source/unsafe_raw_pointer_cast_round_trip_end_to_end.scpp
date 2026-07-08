int main() {
    int x = 7;
    int* ip = &x;
    [[scpp::unsafe]] {
        void* erased = static_cast<void*>(ip);
        int* restored = static_cast<int*>(erased);
        *restored = 11;
    }
    print_int(x);
    return 0;
}
