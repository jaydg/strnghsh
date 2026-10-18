# strnghsh

A small C++20 module for hashed string identifiers. An identifier holds a 64-bit
FNV-1a hash and a database pointer: cheap to copy and compare, with the original
text available for logging. The database detects different strings with the same
hash instead of silently accepting a collision.

```cpp
import <iostream>;
import <optional>;
import strnghsh;

int main()
{
    using namespace strnghsh::literals;
    strnghsh::string_id player("player");
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

`string_id("player")` uses `default_database()`, a shared, lazily initialized,
thread-safe name table. Prefixes and generators keep using their prefix's database.
The library owns this table and retains all its strings until program shutdown;
do not access its identifiers or views after the table is destroyed during shutdown.
For scoped storage, continue to pass a database explicitly: `string_id("player", db)`.
For status-returning construction, pass `default_database()` explicitly as well.

This convenience keeps the same representation and equality rules: `string_id`
typically occupies 16 bytes on 64-bit platforms (the size is not guaranteed).
For dense per-entity data, store `hash_type` from `id.hash_code()` after registering
the text in the shared table. Recover text with `default_database().lookup(value)`.
A bare hash occupies 64 bits and carries no database identity; use it only where
the intended table is known. Creating hashes directly with `hash()` or `"text"_id`
does not register text or check collisions.

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

## Concurrent use

Share one `database` when identifiers created by different threads should compare
equal. Every insertion and lookup takes the same database mutex; duplicate and
collision checks happen inside that lock. Prefix composition is safe too: it
reads immutable prefix text, then inserts the concatenation. Comparisons and
`hash_code()` do not access the database or acquire locks. Heavy concurrent lookup
or insertion can contend on the mutex; independent databases avoid that contention,
but identifiers from different databases compare unequal even for identical text.

| Component | Concurrent access |
| --- | --- |
| `database` | Share freely for insertion and lookup. |
| `string_id` | Read, compare, and copy a published identifier concurrently. Synchronize assignment to an object that other threads access. |
| Stored string views | Read without locking while the database lives; insertions and rehashes preserve them. |
| `counter_generator` | Share `operator()()` and `discard()` with a thread-safe database. Completion order is unspecified; counters can be skipped by retries or discard. |
| `random_generator` | Use a separate instance per thread, or protect every call to `operator()()` and `discard()` with the same external mutex. |
| `dummy_database` | Stateless and safe to share; uniqueness and collisions are not checked. |
| Custom `basic_database` | Supply synchronization in the implementation or around all accesses, including identifier construction and `string()`. |

Construct shared objects before starting workers and keep the database alive until
all workers, identifiers, and string views are finished. Publish identifiers using
normal C++ synchronization, such as thread creation, futures, or a protected queue.
The database mutex does not protect your identifier variables, input strings,
output containers, or shared `insert_status` arguments. Input text must remain
valid and unchanged for the duration of a call; use a separate status variable
per call. Catch exceptions inside a `std::thread`/`std::jthread` worker, since an
exception escaping it calls `std::terminate`. Futures provide another option:
`std::async` transports worker exceptions to `future::get()`.

### Share a counter generator

This complete example creates identifiers in four workers and prints their text
in the calling thread. The futures are declared after the generator and database,
so outstanding async work finishes before either shared object is destroyed,
including when a call to `get()` throws. The `main` handler reports exceptions.

```cpp
import <exception>;
import <future>;
import <iostream>;
import <optional>;
import <vector>;
import strnghsh;

int main() try
{
    strnghsh::database db;
    strnghsh::string_id prefix("entity-", db);
    strnghsh::counter_generator generator(prefix);
    std::vector<std::future<strnghsh::string_id>> jobs;

    for (int worker = 0; worker < 4; ++worker) {
        jobs.push_back(std::async(std::launch::async, [&generator] {
            return generator();
        }));
    }

    for (auto& job : jobs) {
        const auto id = job.get();
        std::cout << id.string().value() << '\n';
    }
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
```

With this initially empty database and unpadded counter, the four names are
`entity-0` through `entity-3`; their printed order depends on scheduling. Generation
retries existing names or hash collisions up to eight attempts, then throws
`generation_error`. Counter operations are atomic but do not synchronize unrelated
application data. Concurrent `discard()` calls can skip values, and a fixed width
or counter wrap can make suffixes repeat.

### Give each worker its own random generator

Keep the database shared for uniqueness checking, but construct the engine and
generator inside each worker. This complete example uses different, reproducible
seeds. Different seeds do not guarantee different names: the shared database
detects duplicates, and the generator retries or throws `generation_error`.

```cpp
import <exception>;
import <future>;
import <iostream>;
import <optional>;
import <random>;
import <vector>;
import strnghsh;

int main() try
{
    strnghsh::database db;
    strnghsh::string_id prefix("task-", db);
    std::vector<std::future<strnghsh::string_id>> jobs;

    for (unsigned worker = 0; worker < 4; ++worker) {
        jobs.push_back(std::async(std::launch::async, [prefix, worker] {
            strnghsh::random_generator<std::mt19937, 12> generator(
                prefix, std::mt19937{worker});
            return generator();
        }));
    }

    for (auto& job : jobs) {
        const auto id = job.get();
        std::cout << id.string().value() << '\n';
    }
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
```

To share a random generator instead, lock an external `std::mutex` with
`std::lock_guard` for the entire `generator()` or `generator.discard(n)` call.
Its database's internal mutex does not protect the engine state.

## Build and test

Requires xmake 3.0.6+ and a compiler with C++20 named module and standard
library header-unit support. Standard library facilities use `import <header>;`
throughout the module, tests, and examples; no C++23 `import std` is required.
The build enables xmake's fallback dependency scanner for Clang so it can discover
header-unit imports before compiling them. Clang currently emits experimental
header-unit warnings.

```sh
xmake f --toolchain=clang -m debug
xmake
xmake test
```

Use `xmake f --toolchain=clang -m release` to select release mode, or
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
Import standard library header units for facilities used by your own code
(for example, `import <optional>;` when comparing optional views). xmake builds
the required header units automatically. There are no third-party dependencies.
Tests cover compile-time hash vectors, binary/bounded input, ownership, rehash
stability, collisions, prefixes, disabled storage, generation, and concurrent access.

The header-unit build is validated with Clang 22.1.8 using libstdc++ 16 and
xmake 3.0.6 on Linux, in debug and release modes (including `NDEBUG`).

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
