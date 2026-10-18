# strnghsh

- Reimplement the ideas of Jonathan Müller's `foonathan/string_id` as a minimal C++20 named module, `strnghsh`.
- Prefer standard library facilities over custom containers, algorithms, utilities, and infrastructure. Keep dependencies and public API small.
- Preserve constexpr 64-bit FNV-1a hashing, recoverable strings, collision detection, prefix composition, and identifier generation.
- Test compile-time and runtime behavior, failure paths, and concurrent database access. Tests must also work with `NDEBUG`.
- Document public code with Doxygen, including ownership, lifetime, thread safety, and failure behavior. Keep README usage and build instructions current.
- Use the zlib license in LICENSE. Retain Jonathan Müller's original notice and credit; clearly identify this as an altered reimplementation.
- Preserve `.astylerc`. Before **every commit**, run `astyle --options=.astylerc src/strnghsh.cppm tests/strnghsh.cpp`, then build and run tests. Do not commit unformatted C++.
- Use xmake as the build system. Configure with `xmake f --toolchain=clang -m debug`, build with `xmake`, and test with `xmake test`. Also check release mode with `xmake f -m release` and `xmake test`.
- Generate and validate documentation with `doxygen Doxyfile`.
