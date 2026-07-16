# Programming a Guessing Game

In the last chapter, you proved that the toolchain works. Now it is time to
write a slightly more interesting program.

We are going to build a small guessing game. The computer will choose one
secret number from 1 to 100. The player will type guesses, and the program
will answer:

- “Too small!” if the guess is below the secret,
- “Too big!” if the guess is above the secret,
- “You win!” when the guess is correct.

This chapter introduces several scpp ideas by using them before we stop to
study them in detail:

- mutable local variables,
- `while` loops,
- `if` conditions,
- checking `std::expected` results,
- importing helper APIs from `scpp`,
- and parsing user input with `std::from_chars`.

## Set up the project

Create a directory, then add `scpp.toml` at the top level and `main.scpp` under
`src/`.

`scpp.toml`:

```toml
manifest-version = 1

[package]
name = "guessing_game"
version = "0.1.0"

[[bin]]
name = "guessing-game"
sources = ["src/**/*.scpp"]
```

## The complete program

`src/main.scpp`:

```cpp
import std;
import scpp;

int main() {
    std::println("Guess the number!");

    int secret_number = scpp::rand::uniform_int_rand(100) + 1;

    while (true) {
        std::println("Please input your guess.");

        auto line_result = scpp::io::getline();
        if (!line_result.has_value()) {
            std::println("Input closed.");
            return 1;
        }
        const std::string& line = line_result.value();
        int guess = 0;
        auto parse_result = std::from_chars(line.c_str(), line.c_str() + line.size(), guess);
        bool parse_failed = static_cast<int>(parse_result.ec) != 0;
        if (parse_failed || parse_result.ptr != line.c_str() + line.size()) {
            std::println("Please enter a whole number between 1 and 100.");
            continue;
        }

        if (guess < 1 || guess > 100) {
            std::println("Please enter a whole number between 1 and 100.");
            continue;
        }

        if (guess < secret_number) {
            std::println("Too small!");
            continue;
        }
        if (guess > secret_number) {
            std::println("Too big!");
            continue;
        }

        std::println("You win!");
        break;
    }

    return 0;
}
```

Build and run it from that directory:

```sh
scpp build
./.scpp/build/*/dev/guessing_game/guessing-game
```

Because the secret number is random, the exact conversation changes every run.
One session's messages might look like this:

```text
Guess the number!
Please input your guess.
Too small!
Please input your guess.
Too big!
Please input your guess.
You win!
```

## What this chapter just introduced

There is a lot packed into this small program.

`scpp::rand::uniform_int_rand(100) + 1` gives us a fresh secret number in the
range 1 through 100. The helper keeps the random-number setup out of the way so
this first interactive program can stay focused on control flow and input
handling.

Each turn calls `scpp::io::getline()`, which also returns `std::expected`. On
success, we get a real `std::string`. If input is closed or fails, we print a
message and stop cleanly.

We turn that string into an integer with `std::from_chars`. It reads decimal
digits directly from the string's character buffer, writes the parsed value
into `guess`, and reports problems through its `ec` field. Here we treat any
non-zero `ec` value as a parse failure, and we also reject any case where the
returned pointer stops before the end of the line, so inputs such as `12abc`
are rejected instead of silently half-parsing.

The `while (true)` loop keeps the game running until one guess is correct. The
`if` chain decides which message to print after each guess.

The important outcome is that you have now written a real interactive program.
It reads input, stores state, repeats work, and branches on conditions. The
next chapter slows down and names those building blocks one by one.

---

[← Previous: Hello, Project Builds](ch01-03-hello-project-builds.md) · [Table of Contents](README.md) · [Next: Variables and Explicit Initialization →](ch03-01-variables-and-explicit-initialization.md)
