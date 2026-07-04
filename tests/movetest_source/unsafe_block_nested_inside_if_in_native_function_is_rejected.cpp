int f(bool cond) {
    if (cond) {
        unsafe {
            return 1;
        }
    }
    return 0;
}
int main() { return f(true); }
