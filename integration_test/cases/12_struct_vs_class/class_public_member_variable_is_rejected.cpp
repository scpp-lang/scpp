// ch04 §4.2: "a member variable -- including a class-level constant --
// can never be public; only member functions can be. Writing public:
// above a member variable is a compile error."
class Widget {
public:
    int value;
};

int main() {
    return 0;
}
