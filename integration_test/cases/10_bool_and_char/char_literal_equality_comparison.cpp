// ch06 scalar table: `char` supports equality comparison (producing a
// `bool`), same as any other scalar.
int main() {
    char a = 'x';
    char b = 'x';
    char c = 'y';
    if (a == b) {
        if (!(a == c)) {
            return 1;
        }
    }
    return 0;
}
