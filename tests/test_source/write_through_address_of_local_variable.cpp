safe void write_via_raw_pointer() {
    int x = 0;
    int* p = &x;
    unsafe {
        *p = 42;
    }
    print_int(x);
    return;
}

int main() {
    write_via_raw_pointer();
    return 0;
}
