struct Foo { int x; };
int f(const Foo& p) {
    const int& r = p.x;
    return r;
}
int main() { return 0; }
