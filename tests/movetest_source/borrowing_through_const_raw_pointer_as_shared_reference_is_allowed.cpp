int f() {
    int x = 5;
    const int* p = &x;
    [[scpp::unsafe]] {
        const int& r = *p;
        return r;
    }
}
int main() { return 0; }
