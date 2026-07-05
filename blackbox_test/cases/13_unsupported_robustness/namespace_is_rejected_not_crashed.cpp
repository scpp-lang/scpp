// ch06/ch08 Q10/ch11 §11.4 backlog: namespace support is "design only so
// far -- today's compiler only ever processes one file at a time", and
// namespaces are specified as part of that same multi-file mechanism.
namespace foo {
    int helper() {
        return 5;
    }
}

int main() {
    return foo::helper();
}
