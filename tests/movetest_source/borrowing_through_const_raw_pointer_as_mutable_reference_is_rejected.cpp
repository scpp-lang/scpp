int f() {
    int x = 5;
    const int* p = &x;
    unsafe {
        int& r = *p;
        r = 9;
    }
    return 0;
}
int main() { return 0; }
