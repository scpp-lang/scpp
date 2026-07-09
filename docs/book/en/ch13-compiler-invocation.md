# 13. Compiler Invocation and CLI

This chapter covers the practical command-line interface of the scpp compiler:
how to compile a program into a native executable, what the common flags do,
and which other subcommands exist besides the ordinary "compile this file"
path.

## 13.1 Default executable build

The default way to compile a program is:

```sh
scpp file.scpp
```

This compiles and links `file.scpp` straight to a native executable.

If no output name is given, the default output file is:

```text
a.out
```

matching the long-standing convention used by compilers such as `clang` and
`gcc`.

To choose an explicit output name, pass `-o`:

```sh
scpp file.scpp -o myprogram
```

So the two most common forms are:

```sh
scpp hello.scpp
scpp hello.scpp -o hello
```

## 13.2 Common flags for executable builds

The executable-producing path accepts the following commonly used flags.

### `-o <output>`

Choose the output executable path explicitly:

```sh
scpp app.scpp -o app
```

Without `-o`, the output defaults to `a.out` ([§13.1](#131-default-executable-build)).

### `-I <dir>`

Add a module search directory.

This is the convenience search mechanism for resolving `import mylib.math;`
without spelling a specific file path on the command line. The search behavior
itself is specified in [ch11 §11.14](ch11-modules-and-libraries.md#1114-importlibrary-search-path).

Example:

```sh
scpp app.scpp -I ./vendor/modules
```

### `--import name=path`

Provide an explicit module-path override.

This is the fully explicit form: instead of searching directories, tell the
compiler exactly which `.scppm` or `.scppkg` should satisfy a particular module
name. The exact lookup rule is also described in
[ch11 §11.14](ch11-modules-and-libraries.md#1114-importlibrary-search-path).

Example:

```sh
scpp app.scpp --import mylib.math=./pkg/mylib.math.scppm
```

### `--link <path>`

Add an extra native link input.

Use this when the final executable needs an additional native object file,
archive, or other linker-visible input beyond what scpp itself already builds
for the current program and its imported modules.

Example:

```sh
scpp app.scpp --link ./native/libhelper.a
```

### `--static`

Request a fully static executable.

Example:

```sh
scpp app.scpp --static -o app
```

Whether that succeeds in practice still depends on the target platform and on
whether static variants of the needed system libraries are actually available --
exactly the same caveat ordinary native toolchains already have.

### `-g`

Emit DWARF debug information into the output binary.

Example:

```sh
scpp app.scpp -g -o app
```

This is what enables source-level debugging with LLDB-based tools such as VS
Code + CodeLLDB. See [ch12](ch12-ide-integration.md) for the editor/debugger
setup itself.

## 13.3 Diagnostic subcommands: `lex` and `parse`

Two keyword subcommands remain useful for diagnostics and compiler-internal
inspection:

```sh
scpp lex file.scpp
scpp parse file.scpp
```

- `lex` dumps the token stream.
- `parse` dumps the AST.

These are primarily inspection/debugging tools for the language and compiler
itself; they are not the ordinary path used to build a runnable program.

## 13.4 Building a precompiled module artifact: `build-module`

Producing a module artifact is a different job from producing an executable, so
it remains an explicit keyword subcommand:

```sh
scpp build-module file.scpp \
  --interface-out file.scppm \
  --archive-out file.scppa
```

This command emits a pair of outputs instead of an executable:

- a `.scppm` module interface file
- a `.scppa` native archive containing the compiled machine code

That artifact model is described in [ch11](ch11-modules-and-libraries.md),
especially the `.scppm` / `.scppa` / `.scppkg` discussion in
[§11.12](ch11-modules-and-libraries.md#1112-the-scppm-scppa-and-scppkg-formats).

The module-building path also accepts the import-related flags used by ordinary
compilation, namely `-I <dir>` and `--import name=path`, for resolving modules
needed while building that artifact.

## 13.5 One-page summary

The main command forms are:

```sh
scpp file.scpp
scpp file.scpp -o output
scpp file.scpp -g
scpp file.scpp -I dir --import name=path --link path --static
scpp lex file.scpp
scpp parse file.scpp
scpp build-module file.scpp --interface-out file.scppm --archive-out file.scppa
```

The key split to remember is simple:

- `scpp file.scpp ...` builds an executable
- `scpp build-module ...` builds a reusable module artifact instead
- `scpp lex ...` / `scpp parse ...` inspect intermediate compiler-facing forms

---

[← Previous: IDE Integration](ch12-ide-integration.md) · [Table of Contents](README.md)
