// ch02: "Raw pointer deref in safe | Must be inside unsafe { }."
safe int f(int* p) {
    return *p;
}

int main() {
    int x = 5;
    return f(&x);
}
