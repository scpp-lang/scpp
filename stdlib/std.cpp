// std.cpp
//
// scpp's "std" module: the primary interface unit. The module's actual
// content (`namespace std { export class string { ... }; }`) lives in a
// separate partition, std_string.cpp (`export module std:string;`, ch11
// §11.4) -- this file just aggregates it via `export import :string;`,
// re-exporting std::string to anyone who writes `import std;`. This is
// scpp's concrete demonstration of both calling into a real C/C++
// library and scpp's own multi-file module system (ch11
// docs/book/en/ch11-modules-and-libraries.md), including module
// partitions specifically: a real module can spread its declarations
// across more than one file without a preprocessor.
export module std;

export import :memory;
export import :string;
