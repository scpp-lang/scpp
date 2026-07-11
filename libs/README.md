# libs — scpp's library modules

This directory contains scpp's shipped library modules: the real-`std` module under `libs/std/`, plus scpp-specific extensions under `libs/scpp/`. Native helper libraries live here too.

The project convention is:

- expose library surface through scpp's real module system (`import std;`)
- keep each area in its own partition-oriented subdirectory (`string/`,
  `memory/`, future ones)
- use ordinary scpp code where possible
- use small native C/C++ wrapper libraries only when a partition needs to
  bridge to existing runtime functionality scpp does not yet implement by
  itself

## Layout

| Path | Role |
|---|---|
| `std/std.scpp` | Primary interface unit of module `std`; re-exports its partitions with `export import :...;` |
| `std/` | Real-C++-mirroring library partitions and native wrappers for module `std` |
| `scpp/scpp.scpp` | Primary interface unit of module `scpp`; re-exports scpp-specific partitions |
| `scpp/rand/` | `scpp:rand` partition with `scpp::rand::uniform_int_distribution` |
| `CMakeLists.txt` | Builds native helper libraries plus the `std` and `scpp` module artifacts |

## Consuming `std`

User code writes:

```cpp
import std;
```

and the build passes the module mappings explicitly, for example:

```sh
scpp app.scpp -o app \
  --import std=libs/std/std.scpp \
  --import scpp=libs/scpp/scpp.scpp \
  --link build/libs/libscpp_string_wrapper.a
```

Notes:

- `libs/std/std.scpp` aggregates the `std` partitions; consumers never import
  `std:string` or `std:memory` directly in source.
- `libs/scpp/scpp.scpp` aggregates scpp-specific partitions; consumers explicitly
  opt in with `import scpp;`.
- `--link` is only needed for partitions with a native helper library.
  Today that means `std:string`; `std:memory` is pure scpp and needs no
  extra native library.
- Partitions compile together with the primary interface unit as one `std`
  module object; there is no textual concatenation.

## Current partitions

### `std:string`

- File: `std/string/std_string.scpp`
- Backed by: `std/string/scpp_string_wrapper.{h,cpp}`
- Provides a small `std::string` surface via `extern "C"` wrapper calls

### `std:memory`

- File: `std/memory/std_memory.scpp`
- Pure scpp implementation
- Provides `std::unique_ptr<T>` and `std::make_unique<T>(...)`

## Testing policy

`libs/` is library source, not a demo area. Coverage belongs in the real
test suites:

- `tests/` for dev-agent-owned unit/integration coverage
- `blackbox_test/` for user-visible language/library behavior

Native helper libraries are built here because the tests and compiler need
them, but demo executables do not live here.
