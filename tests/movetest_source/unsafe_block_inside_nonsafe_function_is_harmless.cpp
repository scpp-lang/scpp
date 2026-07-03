int not_safe() {
    return 1;
}
int f() {
    unsafe {
        return not_safe();
    }
}
