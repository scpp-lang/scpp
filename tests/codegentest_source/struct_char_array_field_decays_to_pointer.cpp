struct S {
    char text[4];
};
void f(char* p) {
    return;
}
int main() {
    S s;
    f(s.text);
    return 0;
}
