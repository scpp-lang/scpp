int helper() {
    return 1;
}
int f() {
    unsafe {
        unsafe {
            return helper();
        }
    }
}
