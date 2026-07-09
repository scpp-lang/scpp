# 12. IDE Integration

This chapter covers the practical side of debugging scpp programs at the
**source level**: breakpoints, stepping, backtraces, and variable inspection
inside VS Code.

The key enabling piece is that invoking `scpp` on a source file with `-g` can emit **real DWARF debug
information** for the generated native binary (see [ch13](ch13-compiler-invocation.md)). Once that information is
present, ordinary LLDB-based tooling can debug a scpp program the same way it
debugs any other LLVM-produced executable.

## 12.1 Building with debug information (`-g`)

Use `-g` when building the program you want to debug:

```sh
scpp foo.scpp -o foo -g
```

`-g` tells `scpp` to emit debug metadata into the output binary. In practical
terms, that is what makes all of the following work:

- setting breakpoints by source line
- stepping through the program statement-by-statement
- inspecting local variables in the current scope
- printing backtraces with real source locations

Without `-g`, the binary still runs normally, but the debugger has much less
source-level information to work with.

## 12.2 VS Code: recommended setup

The simplest VS Code setup today is the **CodeLLDB** extension:

- extension ID: `vadimcn.vscode-lldb`
- marketplace name: **CodeLLDB**

This is the recommended path because it bundles its own LLDB and already works
well with LLVM/DWARF binaries, so there is very little extra setup.

## 12.3 Minimal `launch.json`

Create `.vscode/launch.json` in your workspace:

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "type": "lldb",
      "request": "launch",
      "name": "Debug scpp program",
      "program": "${workspaceFolder}/myprogram",
      "cwd": "${workspaceFolder}"
    }
  ]
}
```

Then:

1. build the binary with `-g`
2. open the workspace in VS Code
3. set breakpoints in the source file
4. start the `Debug scpp program` launch configuration

Adjust `program` to the path of the binary you actually built.

## 12.4 The `.scpp` breakpoint-gutter gotcha

There is one important VS Code-specific gotcha.

By default, breakpoints usually show up immediately for `.cpp` files, but **not
for `.scpp` files**. The debugger itself is not the problem: once launched
against a `-g` binary, LLDB can debug the program correctly. The issue is the
**editor-side breakpoint gutter**.

VS Code only shows/allows ordinary source breakpoints for language modes that
an installed debug extension has declared as breakpoint-capable. CodeLLDB
already registers common languages such as `cpp`, `c`, and `rust`, but
naturally it knows nothing about a separate `scpp` language mode. Meanwhile,
`debug.allowBreakpointsEverywhere` defaults to `false`, so VS Code does not
show the breakpoint gutter for unknown language IDs.

## 12.5 Recommended fix: associate `*.scpp` with `cpp`

The recommended fix is to add a **workspace-local** file association in
`.vscode/settings.json`:

```json
{
  "files.associations": {
    "*.scpp": "cpp"
  }
}
```

This makes VS Code treat `*.scpp` files as language ID `cpp` for tooling
purposes. Since CodeLLDB already declares `cpp` as breakpoint-capable, the
breakpoint gutter appears and normal line breakpoints work in `.scpp` files.

This also has a useful side effect: the file gets approximate C++ syntax
highlighting, which is generally a good fit for scpp source anyway.

## 12.6 Why this fix is preferred

An alternative is the global setting:

```json
{
  "debug.allowBreakpointsEverywhere": true
}
```

That works more broadly, but it is a much blunter tool: it enables the
breakpoint gutter for **every** file type, not specifically for scpp. The
`files.associations` approach is therefore the better default recommendation:

- scoped to `*.scpp`
- works with CodeLLDB's existing `cpp` breakpoint support
- improves syntax highlighting at the same time

## 12.7 Minimal end-to-end recipe

A practical minimal setup looks like this:

1. Install **CodeLLDB** (`vadimcn.vscode-lldb`).
2. Build with debug info:
   ```sh
   scpp myprogram.scpp -o myprogram -g
   ```
3. Add `.vscode/launch.json` with an `lldb` launch configuration.
4. Add `.vscode/settings.json` with:
   ```json
   {
     "files.associations": {
       "*.scpp": "cpp"
     }
   }
   ```
5. Open `myprogram.scpp`, place breakpoints, and launch the debugger.

Once this is in place, VS Code can provide ordinary source-level debugging for
scpp programs: breakpoints, stepping, variable inspection, and backtraces.

---

[← Previous: Modules & Libraries](ch11-modules-and-libraries.md) · [Table of Contents](README.md) · [Next: Compiler Invocation and CLI →](ch13-compiler-invocation.md)
