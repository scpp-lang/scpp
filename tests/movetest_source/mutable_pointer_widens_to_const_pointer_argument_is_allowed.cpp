int read_via_ptr(const int* p) { return 0; }
int f() {
    int x = 5;
    int* p = &x;
    return read_via_ptr(p);
}
int main() { return 0; }
