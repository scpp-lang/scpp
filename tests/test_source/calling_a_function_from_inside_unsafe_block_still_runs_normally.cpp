int helper() {
    print_int(7);
    return 0;
}
int f() {
    unsafe {
        return helper();
    }
}
int main() {
    return f();
}
