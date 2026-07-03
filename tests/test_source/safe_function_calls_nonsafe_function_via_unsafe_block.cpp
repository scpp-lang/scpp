int not_safe() {
    print_int(7);
    return 0;
}
safe int f() {
    unsafe {
        return not_safe();
    }
}
int main() {
    return f();
}
