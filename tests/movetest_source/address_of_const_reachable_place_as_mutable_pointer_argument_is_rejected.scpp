struct Foo { int x; };
void mutate_via_ptr(int* p) { *p = 99; }
int f(const Foo& p) {
    mutate_via_ptr(&p.x);
    return 0;
}
int main() { return 0; }
