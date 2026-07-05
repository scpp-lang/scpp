// ch02 boundary table: "safe calls unsafe | Must be wrapped in
// unsafe { }, otherwise a compile error."
int not_safe() {
    return 1;
}

safe int caller() {
    return not_safe();
}

int main() {
    return caller();
}
