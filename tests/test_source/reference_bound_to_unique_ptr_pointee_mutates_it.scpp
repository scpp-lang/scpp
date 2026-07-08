import std;
int inc(int& v) {
    v = v + 1;
    return 0;
}

int main() {
    std::unique_ptr<int> p = std::make_unique<int>(5);
    int& r = *p;
    inc(r);
    print_int(*p);
    inc(*p);
    print_int(*p);
    return 0;
}
