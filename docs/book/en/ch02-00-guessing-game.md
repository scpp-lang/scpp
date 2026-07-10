# Programming a Guessing Game

In the last chapter, you proved that the toolchain works. Now it is time to
write a slightly more interesting program.

We are going to build a small guessing game. The computer will keep one secret
number. The player will type guesses, and the program will answer:

- “Too small!” if the guess is below the secret,
- “Too big!” if the guess is above the secret,
- “You win!” when the guess is correct.

This chapter introduces several scpp ideas by using them before we stop to
study them in detail:

- mutable local variables,
- `while` loops,
- `if` conditions,
- calling C functions through `extern "C"`,
- and using `[[scpp::unsafe]]` to mark those calls explicitly.

For now, we will keep the secret number fixed at `42`. That keeps the program
small and lets us focus on the core language mechanics first.

## The complete program

Create a file named `guessing_game.scpp`:

```cpp
extern "C" int puts(const char* s);
extern "C" int scanf(const char* fmt, ...);

int main() {
    int secret = 42;
    int guess = 0;

    [[scpp::unsafe]] {
        puts("Guess the number!");
    }
    while (guess != secret) {
        [[scpp::unsafe]] {
            puts("Please input your guess:");
            scanf("%d", &guess);
        }
        if (guess < secret) {
            [[scpp::unsafe]] {
                puts("Too small!");
            }
        } else {
            if (guess > secret) {
                [[scpp::unsafe]] {
                    puts("Too big!");
                }
            } else {
                [[scpp::unsafe]] {
                    puts("You win!");
                }
            }
        }
    }
    return 0;
}
```

Build and run it:

```sh
scpp guessing_game.scpp -o guessing_game
printf '25\n50\n42\n' | ./guessing_game
```

Output:

```text
Guess the number!
Please input your guess:
Too small!
Please input your guess:
Too big!
Please input your guess:
You win!
```

If you run the program interactively instead of piping input into it, just type
one guess per line.

## What this chapter just introduced

There is a lot packed into this small program.

`int secret = 42;` and `int guess = 0;` introduce local variables. The value in
`guess` changes over time, so the loop can keep trying until the player gets
the answer right.

The `while (guess != secret)` loop keeps running until the condition becomes
false. Inside it, the program asks for another guess and compares that guess
with the secret number.

The `if` / `else` chain chooses which message to print. This is the first time
we have used branching to make the program react differently based on data the
user entered.

The calls to `puts` and `scanf` come from the C runtime, not from scpp itself,
so each call sits inside an explicit `[[scpp::unsafe]]` block. That does not
mean the whole program is “unsafe.” It means the exact boundary where you trust
external code is visible and local.

## Why write the program this way?

If you know Rust, you may notice that this version does not yet use a safe
string-based input API or a random-number library. That is intentional: this
chapter stays inside what scpp supports cleanly today.

The important outcome is that you have now written a real interactive program.
It reads input, stores state, repeats work, and branches on conditions. The
next chapter slows down and names those building blocks one by one.

---

[← Previous: Hello, Project Builds](ch01-03-hello-project-builds.md) · [Table of Contents](README.md) · [Next: Common Programming Concepts →](README.md)
