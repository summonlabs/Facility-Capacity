# Facility Capacity 1.0.0 — validation record

This file records what was actually executed and what it produced. It contains
no projections and no roadmap. Everything below was run on the machine described
in §1, against the commit named in §9.

---

## 1. Environment

| Item | Value |
| --- | --- |
| Platform | Windows, x64, 16 logical processors |
| Compiler | MSVC 19.44.35207 (Visual Studio 2022 Community) |
| CMake | 4.3.2 |
| Generator | Ninja 1.13.2 |
| Host | single machine, single process unless stated |

No hardware integration of any kind was exercised. Nothing in this validation
touches a PDU, UPS, generator, BMS, chiller, switch, NIC or accelerator. All
values in every test and benchmark are **SYNTHETIC**.

## 2. Builds

| Configuration | Command | Result |
| --- | --- | --- |
| Release | `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel` | 0 errors, **0 warnings** under `/W4 /WX /permissive-` |
| Debug | `cmake -S . -B build-debug-full -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build-debug-full --parallel` | 0 errors, **0 warnings** under `/W4 /WX /permissive-` |
| x86 Release | `vcvars32` + Ninja, `-DFCAP_ENABLE_ASAN=ON` | 0 errors, **0 warnings** |
| Sanitizer x64 | `-DFCAP_ENABLE_ASAN=ON` on x64 | **blocked** — see §6 |

First-party warnings are never globally suppressed. The warning interface is a
`BUILD_INTERFACE`, so a consumer of the installed package does not inherit
first-party build flags.

Two genuine first-party defects were found by validating more than one
configuration and were fixed:

* `tests/test_multiprocess.cpp` — two child entry points ended on a
  non-returning call that the debug CRT does not annotate as `noreturn`; the
  Debug build failed with `C4716` while Release passed.
* `tests/test_property.cpp` — two implicit `uint64_t` → `size_t` conversions
  that are harmless on x64 and are `C4244` on x86. Found only because the x86
  sanitizer build was attempted.

## 3. Tests

One executable, `fcap_tests`, because the multiprocess suite re-enters the same
binary as a child process.

```
ctest --test-dir build --output-on-failure
   1/1 Test #1: fcap_tests ....... Passed  2.69 sec
   100% tests passed, 0 tests failed out of 1
```

| Configuration | Result |
| --- | --- |
| Release, seed 364355035691300 | `tests=363 failures=0` |
| Debug, seed 364397665026200 | `tests=363 failures=0` |
| x86 + AddressSanitizer, seed 364523379162200 | `tests=363 failures=0`, 0 AddressSanitizer reports |

**No test carries a timeout of any kind.** There is no CTest `TIMEOUT`, no shell
timeout wrapper, no watchdog success logic, no process time limit, and no forced
termination classified as a pass. Both Debug and Release runs complete in about
three seconds.

One hang was encountered during development and was diagnosed rather than
worked around: a test built a carriage-return-injected document with a forward
scan that re-found the newline it had just inserted, so the loop never
terminated. It was a defect in the test, not in the library, and it was fixed by
scanning backwards. The suite has never hung since.

### 3.1 Suite coverage

| Suite | Cases | What it establishes |
| --- | --- | --- |
| `units`, `measured`, `dimension` | 55 | exact integer arithmetic, unit-mismatch refusal, unknown-is-not-zero |
| `identity`, `generation`, `provenance` | 41 | identifier alphabet, no cross-family conversion, checked counters, window semantics |
| `digest` | 16 | SHA-256 against the published FIPS 180-4 vectors, including the one-million-character vector, and incremental/one-shot agreement |
| `evidence` | 19 | every rule `SourceEvidence::create` enforces, including exact closure |
| `reserve`, `constraint`, `reason` | 62 | declaration validation, window semantics, digest stability, complete name round-trips |
| `derivation` | 18 | outcome precedence, exact closure, limiting attribution, completeness |
| `model` | 22 | preconditions, validation precedence, bounded idempotency, epoch rules, canonical round trip |
| `revalidate` | 13 | every rejection class and its severity ordering |
| `canonical` | 10 | determinism across input orderings, round trip, tamper refusal, escaping |
| `diff`, `explain` | 10 | exact diffs, pure deterministic ASCII renderings |
| `store`, `store_format` | 41 | manifests, fencing, verify, prune, residue, corrupt and swapped state |
| `recovery` | 18 | close/reopen, one whole authoritative state, every header rejection path |
| `property` | 5 | conservation over randomized inputs against an independent reference model |
| `adversarial` | 10 | malformed and hostile input, path attacks, oversized declarations, overflow |
| `concurrency` | 7 | shared/exclusive discipline under threads |
| `multiprocess` | 8 | real processes, real locks, real fencing, real crash points |
| `cli` | 3 | the built tool driven as a process through a full lifecycle |

### 3.2 Property tests

The central claim — that capacity is conserved and cannot be created from
nowhere — is tested by building random facilities and comparing the published
answer with an **independent reference model** that recomputes totals from the
raw inputs using plain loops and plain integers, without calling the library's
composition code or its accounting helpers. Agreement is evidence, not
tautology.

Asserted over randomized inputs: composed totals equal the reference exactly;
`allocatable <= usable <= installed`; both closure identities hold on the
published answer; a change that only ever reduces installed or usable capacity,
or increases unavailable, protected or reserved capacity, never increases
allocatable; withholding any part of the allocation identity produces `unknown`
and never zero; and shuffling the input order produces byte-identical encodings
and identical digests.

### 3.3 Real multiprocess and crash-recovery results

Every item below was produced with independent operating-system processes, not
threads, and with real `LockFileEx` file locks:

| Property | Result |
| --- | --- |
| A second writer cannot open while one process holds the exclusive lock | refused with `lock_conflict` |
| A reader cannot open while a writer holds the exclusive lock | refused with `lock_conflict` |
| Process death relinquishes the lock | the next open succeeds after the child is terminated |
| Epoch fencing across processes | a process committing under a superseded epoch is refused with `stale_authority` |
| Generation fencing across processes | a process committing against a superseded generation is refused with `stale_generation` |
| Crash between staging and publication | the previously committed generation stays authoritative; the staging file is reported as residue; `verify` is still `ok`; a later commit succeeds |
| Crash immediately after a commit returns | the committed generation is durable and recovers |
| Four concurrent writers × ten commits | 40 generations, no gap, no duplicate, `verify(true)` `ok`, `recover()` succeeds; only `lock_conflict`, `stale_generation` and `stale_authority` were ever observed |

## 4. Install, export and downstream consumer

```
cmake --install build --prefix <prefix>
```

Installs `bin/fcap.exe`, `include/dccp/facility_capacity/*.hpp`, the static
library, the versioned CMake package
(`lib/cmake/facility_capacity/facility_capacityConfig.cmake` plus
`…ConfigVersion.cmake` and the `facility_capacityTargets.cmake` export set
providing `dccp::facility_capacity`), and `LICENSE`, `README.md` and `NOTICE`
under `share/doc/FacilityCapacity`.

An independent out-of-tree project, `tests/downstream/`, was configured against
the installed prefix only:

```
cmake -S tests/downstream -B <dl> -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<prefix>
cmake --build <dl> --parallel
```

It includes only the installed umbrella header and links only the exported
target. Observed output:

```
facility_capacity version 1.0.0
canonical format version 1, store format version 1
committed generation 1
snapshot digest eaf14fd451a1f86c46688cb201fea977991a55cb0e24506e7d4580af94a5a658
recovered freshness recovered
outcome usable allocatable 1500000 mW
verify ok, 3 files checked
pruned 0 file(s)
downstream consumer completed
exit=0
```

## 5. Examples and command-line tool

`fcap_example_quick_start.exe` exits 0 and prints the published answer for both
generations plus a structural diff, including
`dimension: power usable=5600000 mW -> 4400000 mW (delta -1200000) … outcome=usable`
and `source: power/power-feed-a present gen=1 -> gen=2 state=nominal -> degraded`.

`fcap_example_durable_lifecycle.exe <dir>` exits 0 through create → publish →
commit → destroy objects → reopen → recover → revalidate → `verify(true)` →
prune, and prints
`note=the recovered snapshot is stamped 'recovered', not 'issued': it must be
revalidated against the current model before it is relied on`.

A full CLI lifecycle was run as real processes:

| Command | Exit |
| --- | --- |
| `fcap store init --path … --facility facility-a --site site-1 --epoch 7 --incarnation 3 --controller controller-a --require power` | 0 |
| `fcap evidence declare --path … --source power-feed-a --dimension power --generation 1 --installed 4000000 --usable 3600000 --unavailable 400000 --reserved 0 --protected 0 --derive-residual` | 0 |
| `fcap query --path … --dimension power` | 0 |
| `fcap store verify --path … --deep` | 0 (`ok=1`, 4 files checked) |
| `fcap revalidate --path …` | 0 (`status=valid`) |
| empty argument list, unknown command, unknown option | 2 (usage) |
| stale or invalid domain operation | 1, with exactly one `error: <code>: <message>` line on stderr and nothing on stdout |

## 6. Sanitizer result

**AddressSanitizer could not be built for x64 on this host.** The MSVC
installation at
`C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207`
contains the AddressSanitizer runtime libraries only for **x86**
(`lib\x86\clang_rt.asan_dynamic_runtime_thunk-i386.lib` and the matching
dynamic library under `bin\Hostx64\x86`). The x64 runtime that `/fsanitize=address`
requests — `clang_rt.asan_dynamic_runtime_thunk-x86_64.lib` — is **not present**,
so the link fails with `LNK1104` for every target. No amount of build
configuration can supply a runtime the toolchain does not ship.

**Strongest available substitute, actually run:** the whole suite was built and
executed as a 32-bit (x86) target with MSVC AddressSanitizer enabled
(`vcvars32` + `-DFCAP_ENABLE_ASAN=ON`, `RelWithDebInfo`), with the ASan runtime
directory on `PATH`:

```
seed=364523379162200 tests=363 failures=0
AddressSanitizer reports: 0
exit=0
```

So the suite **is** sanitizer-clean under MSVC AddressSanitizer, on the
architecture for which this installation ships the runtime. What is *not*
claimed: an x64 sanitizer run. The x64 address-space and 64-bit-specific
behaviour were not sanitized here.

The Debug run additionally exercises the MSVC debug runtime's iterator, heap and
stack-frame checks.

## 7. Benchmarks

Methodology (`benchmarks/capacity_benchmarks.cpp`):

* every measurement is the wall time of one **completed** operation; nothing is
  timed before it is submitted;
* the durable figures include the whole commit protocol — staging write, flush,
  re-derive verification, atomic publication of the generation file, directory
  flush, atomic replacement of `CURRENT`, second directory flush;
* one warm-up round, then nine measured rounds; the reported figure is the
  **median of the per-round means**, with the minimum and maximum round means
  beside it so the spread is visible; the machine was shared with no other
  workload of ours during the run;
* all data is generated locally and is **SYNTHETIC**; this is a single host,
  single process, single thread measurement of a control-plane model, not a
  hardware, network or storage-device measurement;
* no before/after pair is published; the two source counts are separate
  absolute measurements, not a comparison that would need a controlled
  alternating methodology to be meaningful;
* the benchmark store is verified with `verify(true)` after the run and then
  removed, so no residue is left.

Run: `build\fcap_benchmarks.exe --rounds 9`, 16 logical processors, Release.

| Operation | median ns | min ns | max ns | completed per round |
| --- | ---: | ---: | ---: | ---: |
| `compose-snapshot-8sources` | 1 433 170 | 972 335 | 7 601 934 | 200 snapshots |
| `encode-snapshot-8sources` | 725 872 | 548 932 | 1 946 293 | 200 documents |
| `decode-and-rerive-snapshot-8sources` | 3 398 158 | 2 560 915 | 4 372 802 | 200 documents |
| `diff-snapshots-8sources` | 6 644 | 4 355 | 9 509 | 200 diffs |
| `compose-snapshot-32sources` | 4 545 869 | 3 802 276 | 20 290 071 | 200 snapshots |
| `encode-snapshot-32sources` | 2 392 051 | 2 148 202 | 3 283 885 | 200 documents |
| `decode-and-rerive-snapshot-32sources` | 12 099 874 | 9 717 621 | 35 472 684 | 200 documents |
| `diff-snapshots-32sources` | 28 703 | 19 518 | 38 701 | 200 diffs |
| `model-query-8sources` | 470 586 | 373 092 | 1 271 598 | 500 answers |
| `model-publish-8sources` | 454 255 | 389 904 | 957 915 | 500 generations |
| `store-commit-durable-8sources` | 16 640 520 | 12 864 580 | 216 264 665 | 20 generations |
| `store-recover-8sources` | 3 392 055 | 3 252 065 | 74 892 980 | 20 generations |

Reading these honestly:

* a full published generation — composing every dimension from 48 evidence
  records, deriving the answer, encoding it and hashing it — completes in about
  1.4 ms, and the durable commit of that generation in about 16.6 ms;
* the durable figure is dominated by the flush and the atomic replacements, as
  it must be: publishing is designed to be durable, not merely fast;
* the wide `max` values on the durable rows are the disk flush tail on a shared
  machine. They are reported rather than hidden, and they are why the median of
  per-round means, not a single run, is the headline figure;
* these are control-plane latencies for evidence-driven republication, which is
  a per-change operation. Nothing here is on a data path.

## 8. Fresh-clone closure

Performed from the configured remote after the release push: clone, configure,
build, test, install, and build and run the downstream consumer against the
freshly installed package. See §9 for the commit and tag that were verified.

## 9. Release identity

Recorded at the moment of the release commit; see the repository log and tags.

---

## 10. Honest limitations

* **No hardware proof of any kind.** Nothing here talks to real facility
  equipment. Every value in every test and benchmark is synthetic.
* **ASan was not run for x64** — the runtime is not shipped by this Visual
  Studio installation. See §6.
* **The POSIX paths are implemented but unexercised.** Validation ran on
  Windows/MSVC. The POSIX branch of the file primitives uses `fcntl`, `fsync`
  and `rename` and is compiled only on non-Windows targets.
* **A store directory is trusted-local.** Its *contents* are untrusted and are
  fully validated, but a process with write access to the directory can delete
  or replace files. The digest is an integrity check, not a signature.
* **The clock is injected.** No wall clock is consulted for an authority
  decision.
* **Idempotency is a bounded window** of the most recent 64 attempt tokens. An
  older token is treated as a new attempt; this is tested and documented.
* **`CapacityStore::authority()` is not synchronised** against a concurrent
  commit from another thread; the documented guarantee is per operation.
* **`begin_commit` releases the exclusive lock before `publish()`**, so a
  competing writer can publish in between. Nothing interleaves — the loser is
  refused — but concurrent writers can leave unreferenced generation files
  behind, which `verify` reports as residue and `prune` removes.
