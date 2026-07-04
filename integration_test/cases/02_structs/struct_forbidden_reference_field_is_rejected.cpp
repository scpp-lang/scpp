// ch04 §4.1: references are forbidden as struct members ("must instead be
// wrapped in a `class`") -- a compile error, not a silent downgrade.
struct Bad {
    int& r;
};

int main() {
    return 0;
}
