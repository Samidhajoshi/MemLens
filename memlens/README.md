# MemLens

A small C++ memory profiler and leak detector. It overrides global
`operator new`/`operator delete` in the file you instrument, tracks
every allocation's size, call site, thread, and timestamp, and flags
leaks, double-frees, and `new[]`/`delete` mismatches. The engine emits
one `memory_report.json`; a standalone `dashboard.html` renders it —
the C++ code doesn't know anything about HTML.

This started as a course project and was hardened into something
closer to a real tool: thread-safe, free of the shell-injection bug
the prototype had, and validated (see "What was actually fixed"
below) rather than just rewritten and hoped for.

## Quick start

### Linux / macOS

```bash
# build the instrumenter
g++ -std=c++17 -pthread src/memlens.cpp -o memlens
# (or: mkdir build && cd build && cmake .. && make)

# profile a program
./memlens samples/leaky_demo.cpp

# view the results
open dashboard.html          # macOS
xdg-open dashboard.html      # Linux
```

### Windows

**PowerShell / Command Prompt, with any MinGW g++ on PATH:**

```powershell
cd memlens
g++ src/memlens.cpp -o memlens
.\memlens.exe samples\mismatch_demo.cpp
start dashboard.html
```

`memlens.exe` finds `memlens.h` itself (it checks `.`, `src`, `../src`,
and next to the executable), so run it from the project root and
either `/` or `\` in the sample path works.

**Recommended toolchain:** [MSYS2](https://www.msys2.org/) with its
`mingw-w64` GCC (`pacman -S mingw-w64-ucrt-x86_64-gcc`, then build
from the `UCRT64` shell). It's modern, actively maintained, and has
full C++17 + threading support out of the box — no fallback paths
needed.

**Older MinGW builds work too**, just with two things handled
automatically rather than by you:

- Some MinGW distributions (the mingw.org-style `mingw32` build, as
  opposed to `mingw-w64`) were compiled without POSIX threading
  support, so `std::mutex`/`std::thread` don't exist in their standard
  library at all. `memlens.h` detects this and transparently falls
  back to native Win32 `CRITICAL_SECTION` locking instead — you don't
  need to do anything, it Just Works, but it's why the header doesn't
  assume `std::thread` is always available.
- Those same toolchains often predate full C++17 support, so
  `memlens.cpp` compiles instrumented files at `-std=c++14` rather
  than `-std=c++17`.

If you hit a build error on Windows anyway, check your `g++ --version`
first — anything older than GCC 7 is the likely culprit, and
switching to MSYS2's mingw-w64 GCC will make problems like this stop
happening entirely rather than needing another one-off fix.

`dashboard.html` tries to `fetch('memory_report.json')` from the same
folder on load; if that fails (e.g. opened directly via `file://` in a
browser that blocks local fetches) it falls back to a bundled sample
report, and you can drag a `memory_report.json` onto the page or use
"Load report" to view it instead.

Two dashboard themes are included — `dashboard.html` (warm/editorial)
and `dashboard-dark-ide.html` (dark developer-tool aesthetic). Both
read the exact same JSON format.

## Troubleshooting

**`[ERROR] File 'x.cpp' not found!`** — you're passing a path that
doesn't exist relative to your current directory. `./memlens
samples/foo.cpp` needs to be run from the folder that contains
`samples/`, not from inside it.

**`fatal error: memlens.h: No such file or directory`** — this
shouldn't happen anymore (the CLI searches `.`, `src`, `../src`, and
next to its own executable), but if it does, run `memlens` from the
project root, or pass the header directory explicitly by compiling
the instrumented file yourself with `-I path/to/src`.

**`'...' declared as an 'inline' variable` / `'mutex' is not a member
of 'std'` / `'std::this_thread' has not been declared`** — an old
GCC/MinGW without full C++17 or threading support. Already handled
automatically as of this version; if you still see it, your `g++` may
predate GCC 6, which is beyond what the fallback covers — upgrade to
MSYS2's mingw-w64 GCC.

**`cannot find -lpthread`** — `-pthread` was being passed to a Windows
compiler that doesn't have a pthread library to link (it doesn't need
one either). Already fixed: `-pthread` is now only passed on
Linux/macOS.

## How it works

Including `memlens.h` does `#define new new(__FILE__, __LINE__)`, so
every `new` expression in that translation unit routes through a
tracked allocator that records size, file, line, thread id, and
timestamp before delegating to `malloc`. Matching `operator delete`
overrides record the deallocation, checking for double-frees and
`new[]`/`delete` mismatches along the way. At exit, anything never
freed is reported as a leak.

`memlens.cpp` is the CLI: it copies your file, inserts the
`#include`, adds a call to generate the report before `return` in
`main()`, compiles it, and runs it.

## What was actually fixed

The starting point already worked — a real `operator new`/`delete`
override, JSON + HTML reports, a working instrumenter. The
improvement pass targeted the specific issues called out in the
project's own review guide, plus two bugs that only showed up once
this was actually compiled and run against real programs rather than
just rewritten by inspection:

- **Thread safety.** All shared state is behind a `std::mutex`, with
  real per-thread ids from `std::this_thread::get_id()`. Verified
  under load: 8 threads, 16,000 concurrent allocations, exact leak
  count, no crashes or corruption.
- **Shell injection.** The instrumenter used to build a shell command
  string from a user-supplied filename and hand it to `system()`. It
  now execs `g++` directly via `fork()`/`execvp()` with an argv array
  (no shell involved at all), plus a filename allow-list as
  defense-in-depth.
- **Double-free / mismatch detection**, sorted with `std::sort`
  instead of bubble sort, as the guide suggested.
- **A self-deadlock I introduced and then found by actually running
  the code**: the profiler's own internal `std::map`/`std::vector`
  cleanup, while holding its lock, was recursing back into itself
  through the global `operator delete` override. Fixed with a
  thread-local reentrancy guard — the same pattern real `LD_PRELOAD`
  allocators use to avoid this exact problem.
- **A silent tracking gap, also only visible at runtime**: GCC's
  sized-deallocation feature (on by default since C++14) means a
  plain `delete p;` resolves to `operator delete(void*, size_t)`, not
  `operator delete(void*)`, whenever it exists — and a *compiler
  provided default* for the sized form exists whether or not you
  define one. Since only the unsized overload was defined, ordinary
  single-object deletes were **never actually being tracked** — only
  `delete[]` was, by accident. Both sized and unsized overloads are
  now defined.
- **A real double-free would otherwise crash the process** before the
  report could be written, since the original design always forwarded
  to the real `free()`. A detected double-free now skips the second
  `free()` call — the block is deliberately leaked rather than
  actually double-freed at the OS level — so the program can finish
  and MemLens can report the bug instead of the process just aborting.
- **Old-MinGW compatibility.** Some MinGW distributions (the
  mingw.org "mingw32" builds, as opposed to modern mingw-w64/MSYS2)
  ship a libstdc++ compiled without POSIX threading support, so
  `std::mutex`/`std::thread` aren't declared at all — not a linker
  issue, a compile-time one — and their GCC predates full C++17
  support (inline variables landed in GCC 7). `memlens.h` now detects
  this via `_GLIBCXX_HAS_GTHREADS` and falls back to native Win32
  `CRITICAL_SECTION`-based synchronization, and the instrumenter
  compiles at `-std=c++14` instead of `-std=c++17` (nothing else in
  the header actually needed C++17). Verified by compiling both code
  paths, not just reasoned about.

All of the above was verified by actually compiling and running
`samples/*.cpp` through the tool, not just written and assumed
correct — see `samples/` for the programs used, including
multi-threaded stress tests.

## What this doesn't catch yet

- Only tracks `new`/`delete` in the file you instrument. STL
  containers, other translation units, and third-party libraries are
  invisible unless you also hook `malloc`/`free` globally (e.g. via
  `LD_PRELOAD` on Linux/macOS or a detours-style hook on Windows) —
  the single biggest upgrade left on the table.
- Tracked by pointer address in one map. If a freed address is reused
  later in the same run, the full allocation list only reflects that
  address's most recent allocation. Aggregate statistics
  (`totalAllocations`, `leakCount`, etc.) stay accurate regardless.
- No stack traces — just the immediate `__FILE__`/`__LINE__` of the
  call site, not the full call chain.
- Single end-of-run snapshot. No periodic sampling (so no real memory
  timeline) and no baseline diff between two runs yet.
- No guard bytes / buffer-overflow detection (the "stretch" tier from
  the original roadmap).
- `CMakeLists.txt` is included but wasn't build-verified in this
  environment (no `cmake` available) — the direct `g++` command above
  was tested and works.

## Files

```
memlens/
  src/
    memlens.h        header-only profiler engine
    memlens.cpp       instrumenter CLI
  samples/
    leaky_demo.cpp        plain unfreed allocations
    double_free_demo.cpp   a pointer freed twice
    mismatch_demo.cpp      new[] freed with delete
    clean_demo.cpp         no bugs, for the "healthy" state
    showcase_demo.cpp      all of the above in one program
  dashboard.html            editorial theme (default)
  dashboard-dark-ide.html   dark developer-tool theme
  memory_report.json        sample report (from showcase_demo.cpp)
  CMakeLists.txt
```



Next, most valuable step: the `LD_PRELOAD` `malloc`/`free` hook, so
MemLens can see allocations inside the STL and any linked library —
not just the one file you point it at.
