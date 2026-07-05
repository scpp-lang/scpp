// ch04 §4.1: raw pointers `T*` are allowed as struct fields ("carry no
// compiler-tracked lifetime; dereferencing still requires unsafe {}").
struct Node {
    int value;
    Node* next;
};

safe int read_value(Node* n) {
    unsafe {
        return n->value;
    }
}

int main() {
    Node a;
    a.value = 5;
    Node b;
    b.value = 9;
    a.next = &b;
    return read_value(a.next);
}
