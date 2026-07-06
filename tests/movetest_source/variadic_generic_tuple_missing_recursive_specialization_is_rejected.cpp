// ch05 §5.14: instantiating a variadic generic type with 1+ arguments
// when only the empty-pack base-case specialization has been declared
// (no recursive-case specialization at all) is rejected -- there is no
// pattern to match a non-empty argument list against.
template<typename... Ts> class Tuple;

template<> class Tuple<> {};

int main() {
    Tuple<int> t;
    return 0;
}
