extern "C" {
    int abs(int n);
}

int compute(int x) {
    unsafe {
        return abs(x);
    }
}

int main() {
    print_int(compute(-42));
    return 0;
}
