int f() {
    int x = 1;
    int* p = &x;
    [[scpp::unsafe]] {
        *p = 99;
    }
    return x;
}
int main() { return 0; }
