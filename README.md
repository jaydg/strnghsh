# strnghsh

A small C++20 module for hashed string identifiers. An identifier holds a 64-bit
FNV-1a hash and a database pointer: cheap to copy and compare, with the original
text available for logging. The database detects different strings with the same
hash instead of silently accepting a collision.

```cpp
#include <iostream>
import strnghsh;

int main()
{
    using namespace strnghsh::literals;
    strnghsh::database db;
    strnghsh::string_id player("player", db);
    strnghsh::string_id weapon(player, "/weapon");

    if (player == "player"_id) {
        std::cout << *weapon.string() << '\n'; // player/weapon
    }
    switch (player.hash_code()) {
    case "player"_id: break;
    }
}
```

The database must outlive its identifiers and returned string views. It owns an
immutable copy of each string; views remain valid across insertions and rehashes.
Identifiers compare equal only when their hashes **and databases** match. Comparing
an identifier with a raw hash checks only the hash. `std::hash<string_id>` supports
standard unordered containers.

`hash(string_view)` is constexpr; `"text"_id` is consteval. Both process all bytes,
including embedded NULs, treating bytes as unsigned. ASCII hashes match upstream.
Hashes are deterministic, non-cryptographic, and unsuitable for secrets. C++20
has no standard stable constexpr string hash, so FNV-1a is implemented here.

`database` uses `std::unordered_map` and `std::mutex` for concurrent insertion and
lookup. It never erases entries. `dummy_database` disables text storage, collision
detection, and uniqueness checking; `string()` then returns `std::nullopt`.
Derive from `basic_database` to provide another storage policy, honoring its view
lifetime contract.

Constructors throw `collision_error` on conflicting text. Overloads with an
`insert_status&` report `new_string`, `old_string`, or `collision` instead. A failed
insertion preserves the existing entry; a status-returning identifier created on
collision refers to that existing text. Allocation and database exceptions propagate.

`counter_generator(prefix, start = 0, width = 0)` appends decimal counters. A nonzero
width pads with zeros or retains the last digits; the 64-bit counter wraps.
`random_generator<Engine, Length>(prefix, engine, alphabet)` appends random bytes
from an owned alphabet (ASCII letters and digits by default). Both retry duplicates
and collisions up to eight attempts, then throw `generation_error`. Counter
generators support concurrent calls with a thread-safe database; random generators
require external synchronization. `discard(n)` advances counter/engine state.

## Build and test

Requires xmake 3.0.6+ and a compiler with C++20 module support. Tested toolchains
are recorded below. Standard library headers are included
in the module's global fragment; no C++23 `import std` is required.

```sh
xmake f --toolchain=clang -m debug
xmake
xmake test
```

Use `xmake f -m release` to select release mode, or
`xmake f -c --toolchain=gcc -m release` to build with GCC. `xmake test` automatically
builds the test executable; the default build produces only the library.
If Clang's scanner cannot find standard headers when a compiler wrapper is on
`PATH`, configure with `--cc=/usr/bin/clang --cxx=/usr/bin/clang++` to select the
compiler executables directly.

Consumers can include the project and depend on its public module:

```lua
set_languages("c++20")
includes("path/to/strnghsh")

target("app")
    set_kind("binary")
    add_deps("strnghsh")
    add_files("main.cpp")
```

Then use `import strnghsh;` in C++.
Include standard headers for standard library facilities used by your own code
(for example, `<optional>` when comparing optional views). There are no third-party dependencies.
Tests cover compile-time hash vectors, binary/bounded input, ownership, rehash
stability, collisions, prefixes, disabled storage, generation, and concurrent access.

Validated with Clang 22.1.8 and GCC 16.2.1 using libstdc++ 16 and xmake 3.0.6
on Linux. GCC was tested in release mode with `NDEBUG`; Clang was also tested
with AddressSanitizer and UndefinedBehaviorSanitizer.

Generate API documentation with `doxygen Doxyfile` (output: `docs/html/index.html`).
Before every commit, format with the tracked configuration and rerun the checks:

```sh
astyle --options=.astylerc src/strnghsh.cppm tests/strnghsh.cpp
```

## Origin and license

This is an altered reimplementation of Jonathan Müller's
[foonathan/string_id](https://github.com/foonathan/string_id), studied at revision
`b5a21bb9e1f8cebc64a3daa2cba16ffce8d0a334`. Credit for the original design and
implementation belongs to Jonathan Müller. The original zlib notice is preserved
verbatim in [LICENSE](LICENSE).

Upstream combines FNV-1a hashes, a database interface, custom chained hash storage,
collision/generation callbacks, and generators. strnghsh retains those core ideas
with standard containers, strings, synchronization, and randomness in one named
module. It is a new API, not a drop-in replacement: `std::string_view` replaces
`string_info`, optional views replace C-string lookup, exceptions replace global
callbacks, and a single synchronized database replaces the map/adapter/configuration
layers. Lengths are honored when hashing (upstream hashes until NUL), including
bounded and binary inputs; bytes above 127 use unsigned-byte FNV-1a semantics.
