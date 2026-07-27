# Compile-and-Run Tests

By the end of Chapter 10, we have enough language machinery to write useful
library code. The next step is to keep that code checked automatically.

This project's first testing layer is the black-box runner under
`blackbox_test/`. It treats `scpp` as an external command-line compiler, builds
small `.scpp` programs, runs them, and compares the observed result with an
expected outcome file.

Assuming the main compiler build already produced `./build/scpp`, build the test
runner like this from the repository root:

```sh
cmake -S blackbox_test -B blackbox_test/build
cmake --build blackbox_test/build
```

## A passing test is just one `.scpp` file plus one `.expected` file

Create a category directory and add one tiny program:

`blackbox_test/cases/99_demo/hello_test.scpp`

```cpp
import std;

int main() {
    std::println("hello from test");
    return 0;
}
```

Then add the expected outcome beside it:

`blackbox_test/cases/99_demo/hello_test.expected`

```text
0
hello from test
```

The first line is the process exit code. Everything after that first newline is
the exact stdout the runner expects.

Now run just that one slice of the suite:

```sh
./blackbox_test/build/run_tests 99_demo
```

Output:

```text
ok   99_demo/hello_test.scpp

1/1 case(s) passed.
```

That is the core pattern for compile-and-run tests in this repository: write a
small program that illustrates one rule, then write the exact outcome it should
have when compiled and executed through `scpp`.

## A negative test uses `COMPILE_ERROR`

Some documented rules are supposed to reject code rather than run it. In that
case, the `.expected` file names that fact directly.

`blackbox_test/cases/99_demo/const_ref_rejects_write.scpp`

```cpp
int main() {
    int value = 7;
    const int& ref = value;
    ref = 9;
    return 0;
}
```

`blackbox_test/cases/99_demo/const_ref_rejects_write.expected`

```text
COMPILE_ERROR
```

Run the same filter again:

```sh
./blackbox_test/build/run_tests 99_demo
```

Output:

```text
ok   99_demo/const_ref_rejects_write.scpp
ok   99_demo/hello_test.scpp

2/2 case(s) passed.
```

`COMPILE_ERROR` does **not** pin down one exact diagnostic string. It only says
the compiler must fail cleanly, with a real error, rather than crash or accept
the program.

## The working model for black-box compile-and-run tests

So far, the practical rules are:

- each ordinary black-box test is a pair of sibling files:
  `<name>.scpp` and `<name>.expected`;
- if `.expected` starts with a number, that number is the expected exit code,
  and the remaining bytes are the expected stdout;
- if `.expected` is `COMPILE_ERROR`, the program must be rejected cleanly;
- the runner accepts a substring filter, so you can re-run one category or one
  small cluster while iterating.

That is enough to write the most common tests in this repository: small programs
that either compile-and-run with a known result or fail to compile for a known
language rule. The next section adds the extra files that control CLI arguments,
output paths, and other command details.

---

[← Previous: Validating References with Lifetimes](ch10-03-validating-references-with-lifetimes.md) · [Table of Contents](README.md)
