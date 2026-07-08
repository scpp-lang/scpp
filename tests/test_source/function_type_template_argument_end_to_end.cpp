template<typename Sig>
class Holder {
public:
    int value;
};

int main() {
    Holder<int(int, int)> h;
    h.value = 11;
    print_int(h.value);
    return 0;
}
