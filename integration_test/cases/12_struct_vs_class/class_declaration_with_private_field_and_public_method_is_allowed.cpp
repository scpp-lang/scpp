// ch04 §4.2: a `private` member variable plus a `public` member function
// is the basic, legal shape of a `class` -- "external code can therefore
// only ever reach a class's data through a method call, never through
// direct field access". This method doesn't touch the field yet (see
// class_method_accessing_private_field_via_this_is_allowed.cpp for that),
// just proving the declaration shape itself and an ordinary method call
// both work.
class Widget {
private:
    int value;
public:
    int get_constant() {
        return 42;
    }
};

int main() {
    Widget w;
    return w.get_constant();
}
