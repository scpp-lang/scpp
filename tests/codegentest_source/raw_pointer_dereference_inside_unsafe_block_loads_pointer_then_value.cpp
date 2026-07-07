int main() {
    int* p;
    [[scpp::unsafe]] {
        int x = *p;
    }
    return 0;
}
