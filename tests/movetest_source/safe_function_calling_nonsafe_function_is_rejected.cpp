int not_safe() {
    return 1;
}
safe int f() {
    return not_safe();
}
