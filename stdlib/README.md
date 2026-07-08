# stdlib — scpp's `std` library

This directory contains scpp's own standard-library implementation: the
`std` module interface unit plus its partitions and any native helper
libraries those partitions need.

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
| `std.scpp` | Primary interface unit of module `std`; re-exports partitions with `export import :...;` |
| `string/` | `std:string` partition plus the native `scpp_string_wrapper` bridge to real C++ `std::string` |
| `memory/` | `std:memory` partition; currently pure scpp (`std::unique_ptr`, `std::make_unique`) |
| `CMakeLists.txt` | Builds native helper libraries needed by stdlib partitions |

## Consuming `std`

User code writes:

```cpp
import std;
```

and the build passes the module mappings explicitly, for example:

```sh
scpp build app.scpp -o app \
  --import std=stdlib/std.scpp \
  --import std:string=stdlib/string/std_string.scpp \
  --import std:memory=stdlib/memory/std_memory.scpp \
  --link build/stdlib/libscpp_string_wrapper.a
```

Notes:

- `std.scpp` aggregates the partitions; consumers never import `std:string`
  or `std:memory` directly in source.
- `--link` is only needed for partitions with a native helper library.
  Today that means `std:string`; `std:memory` is pure scpp and needs no
  extra native library.
- Partitions compile together with the primary interface unit as one `std`
  module object; there is no textual concatenation.

## Current partitions

### `std:string`

- File: `string/std_string.scpp`
- Backed by: `string/scpp_string_wrapper.{h,cpp}`
- Provides a small `std::string` surface via `extern "C"` wrapper calls

### `std:memory`

- File: `memory/std_memory.scpp`
- Pure scpp implementation
- Provides `std::unique_ptr<T>` and `std::make_unique<T>(...)`

## Testing policy

`stdlib/` is library source, not a demo area. Coverage belongs in the real
test suites:

- `tests/` for dev-agent-owned unit/integration coverage
- `blackbox_test/` for user-visible language/library behavior

Native helper libraries are built here because the tests and compiler need
them, but demo executables do not live here.
