struct Foo { int x; };
void mutate(int& x) { x = 99; }
int f(const Foo& p) {
    mutate(p.x);
    return 0;
}
int main() { return 0; }
