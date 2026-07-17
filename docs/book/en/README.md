# The SCPP Programming Language

- [Getting Started](ch01-00-getting-started.md)
  - [Building the Compiler](ch01-01-building-the-compiler.md)
  - [Hello, World!](ch01-02-hello-world.md)
  - [Hello, Project Builds](ch01-03-hello-project-builds.md)

- [Programming a Guessing Game](ch02-00-guessing-game.md)

- Common Programming Concepts
  - [Variables and Explicit Initialization](ch03-01-variables-and-explicit-initialization.md)
  - [Scalar Data Types](ch03-02-scalar-data-types.md)
  - [Functions](ch03-03-functions.md)
  - [Comments](ch03-04-comments.md)
  - [Control Flow](ch03-05-control-flow.md)

- Understanding Ownership
  - [What Is Ownership?](ch04-01-what-is-ownership.md)
  - [References and Borrowing](ch04-02-references-and-borrowing.md)
  - [`std::span` and Other Non-Owning Views](ch04-03-std-span-and-other-non-owning-views.md)

- Using Structs and Classes to Structure Related Data
  - [Defining and Instantiating `struct` and `class`](ch05-01-defining-and-instantiating-struct-and-class.md)
  - [An Example Program Using a Checked Class](ch05-02-an-example-program-using-a-checked-class.md)
  - [Methods and `this`](ch05-03-methods-and-this.md)

- Safety Boundaries and `[[scpp::unsafe]]`
  - What `[[scpp::unsafe]]` Does and Does Not Do
  - Calling `extern "C"` and Using Raw Pointers
  - Localizing Trust in Real Programs

- Packages, Modules, and Project Layout
  - Packages and Project Manifests
  - Control Scope and Privacy with Modules
  - Paths for Referring to Items in the Module Tree
  - Using `import` and Qualified Names
  - Separating Modules into Different Files

- Arrays, Buffers, and Views
  - Fixed-Size Arrays
  - Text as `char` and C-Compatible Buffers
  - Borrowed Views with `std::span`

- Error Handling
  - Unrecoverable Errors and Compiler-Inserted Checks
  - Recoverable Errors Today
  - Preparing for `std::expected`

- Generic Code, Concepts, and Lifetimes
  - Generic Data Types
  - Defining Shared Requirements with Concepts
  - Validating References with Lifetimes

- Writing Automated Tests
  - Compile-and-Run Tests
  - Controlling Test Commands
  - Test Organization

- An I/O Project: Building a Command-Line Program
  - Accepting Command-Line Arguments
  - Reading a File
  - Refactoring into Modules
  - Adding Functionality with Tests
  - Working with Environment Variables
  - Writing Diagnostics to Standard Error

- Closures and Explicit Iteration
  - Closures
  - Processing Sequences with Loops and Views
  - Improving Our Command-Line Project
  - Performance of Explicit Loops

- More about Project Builds and Reusable Packages
  - Compiler and Project Build Modes
  - Building Reusable Module Artifacts
  - Workspaces
  - Installing and Running Binaries
  - Extending the Tooling

- Smart Pointers and Owned Handles
  - Using `std::unique_ptr<T>`
  - Treating Owning Pointers Like References
  - Running Cleanup Code with Destructors
  - `std::shared_ptr<T>`
  - Interior Mutability with `mutable`
  - Avoiding Reference Cycles and Ownership Confusion

- Fearless Concurrency
  - Using Threads to Run Code Simultaneously
  - Moving Data Across Thread Boundaries Safely
  - Shared-State Concurrency
  - Thread Traits: `thread_movable` and `thread_shareable`

- Interoperability and Fixed-Layout Data
  - Fixed-Layout `struct` Values
  - C ABI Boundaries
  - Packed Layouts and `union` Escape Hatches

- Advanced Features
  - Advanced Concepts and Constraints
  - Advanced Types and Function Pointers
  - Advanced Functions and Closures
  - Metaprogramming Without Macros

- Final Project: Building a Multithreaded Web Server
  - Building a Single-Threaded Web Server
  - From Single-Threaded to Multithreaded
  - Graceful Shutdown and Cleanup

- Appendix
  - A - Attributes and Reserved Spellings
  - B - Operators and Symbols
  - C - Standard Library Building Blocks
  - D - Useful Development Tools
  - E - SCPP26 and the C++26 Baseline
  - F - Translations of the Book
