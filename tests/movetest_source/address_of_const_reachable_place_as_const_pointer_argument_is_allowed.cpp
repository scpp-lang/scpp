struct Foo { int x; };
int read_via_ptr(const int* p) { return 0; }
int f(const Foo& p) {
    return read_via_ptr(&p.x);
}
int main() { return 0; }
