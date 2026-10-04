# Facility Capacity

Facility Capacity is the aggregate, generation-bound facility-capacity model.
It composes typed, immutable,
generation-stamped evidence from the space, rack, power, cooling,
operational-reserve and facility-service dimensions into **one** authoritative
answer, and it decides when that answer must be rejected as stale or incomplete.

* **Version:** 1.0.0
* **Language:** portable C++20
* **Build:** CMake, no third-party dependency
* **Primary exercised platform:** Windows / MSVC 19.44, Release and Debug
* **Licence:** Apache License 2.0
* **Telemetry:** none, of any kind

---

## 1. The question this runtime answers

> What facility capacity is actually usable **now**, under which physical
> constraints, reserves, service obligations, evidence and generation, and when
> must that answer be rejected as stale or incomplete?

Everything in this repository exists to make that question answerable with
evidence, and to make a *negative* answer as precise as a positive one. The
model distinguishes, in code and not only in prose:

| Confused in ordinary management systems | Distinguished here |
| --- | --- |
| not measured | zero |
| unmeasured | unavailable |
| offered but unknown | offered and exhausted |
| incomplete coverage | complete coverage with an unmeasured value |
| current | recovered from disk |
| authoritative | merely retained |
| capacity | authority to consume it |

## 2. Exact systems boundary

### What this repository owns

The **aggregate facility-capacity model** for one facility:

* the coverage contract — which dimensions the aggregate must answer, and which
  sources must be present for that answer to count as complete;
* the exact composition of the six dimensions from typed evidence, with per-unit
  integer accounting that closes exactly;
* the published, immutable, content-addressed, generation-bound snapshot;
* reserves and service constraints **as declarations consumed from their
  owners**, composed into the answer;
* degraded-state reporting, limiting-constraint attribution, residuals and
  stable reason codes;
* deterministic publication, query, diff, explanation and revalidation;
* durable, integrity-checked, generation-addressed persistence of the whole
  authoritative state, with real restart recovery and epoch/incarnation fencing.

### What this repository does not own, and does not implement

It is **not** Rack Capacity, Space Capacity, Power Capacity, Cooling Capacity,
Facility Placement Planner, Facility Capacity Reservation or Capacity
Reconciliation. Their policy, derivation rules, lifecycle and inventories stay
in those runtimes. Facility Capacity consumes their published evidence as
immutable typed values and never reimplements their policy.

It does **not**: schedule accelerator workloads or reserve accelerator memory
(Accelerated Systems Infrastructure); choose network paths or reserve fabric
bandwidth (Distributed Fabric Infrastructure); place hardware; actuate
electrical equipment; control cooling equipment; mutate any source inventory;
create, admit, release or schedule a reservation; reconcile capacity against
observed consumption.

A source identity such as `power-capacity` is an **opaque reference**. This
repository never validates that the referenced source exists, only that the
evidence it supplies is internally consistent, correctly typed, correctly
generated and bound to the right facility, site, epoch and incarnation.

## 3. Domain architecture

### 3.1 Dimensions and exact units

A dimension is a distinct kind of facility capacity with exactly one canonical
exact unit. There is **no floating point anywhere in authoritative accounting**.

| Dimension | Canonical unit | Meaning |
| --- | --- | --- |
| `space` | `rack_unit` | usable vertical rack space, one unit = 44.45 mm |
| `rack` | `rack_slot` | countable rack or rack-position capacity |
| `power` | `milli_watt` | electrical power available to IT load |
| `cooling` | `milli_watt_thermal` | heat removal available to IT load |
| `operational_reserve` | `reserve_quantum` | exact count of declared callable operational reserve |
| `facility_service` | `milli_watt` | electrical capacity committed to facility services |

`reserve_quantum` is deliberately opaque: the quantum's physical meaning is owned
by the operational-reserve source. Facility Capacity composes the exact count and
never reinterprets it as power, space or cooling, so operational reserve can
never be silently conflated with a physical dimension.

Dimensions are **never summed together**. A total is always within one dimension
in that dimension's unit; combining quantities in different units is a typed
failure (`unit_mismatch`), not a number.

### 3.2 Quantities: measured or unmeasured, never guessed

Every quantity is a `Measured`: either an exact non-negative integer in the
dimension's unit, or an explicit statement that it was not measured. There is no
code path that reads a zero out of an unmeasured value — `Measured::magnitude()`
throws `ResultMisuse` on an unmeasured value rather than returning a number.

Arithmetic propagates the absence: anything combined with an unmeasured value is
unmeasured. A composed total is therefore never quietly reduced to zero.

The quantities modelled per source and dimension, and at every roll-up:

`installed`, `observed`, `usable`, `protected`, `reserved`, `unavailable`,
`residual`, `allocatable`.

### 3.3 The accounting laws

Per source and per dimension, enforced when evidence is created:

```
installed == usable + unavailable + residual          physical closure
usable    == protected + reserved + allocatable       allocation closure
```

and therefore, over every source that contributes:

```
allocatable <= usable <= installed
```

`installed`, `usable` and `unavailable` are declared by the source. The
`residual` is either declared or derived as `installed - unavailable - usable`;
a derived residual that would be negative is refused. Closure is *proven* at
evidence creation, and re-proven at composition, where a violated identity is an
`invariant_violation` rather than a published number.

The consequence, and the central claim of this runtime: **capacity cannot appear
from nowhere.** The most a dimension can report is what its sources measured as
installed, minus what they measured as unavailable, residual, protected and
reserved. Section 8 records the property tests that establish this over
randomized inputs.

### 3.4 Evidence

`SourceEvidence` is the only way capacity enters the model. It is created once,
validated at creation, digest-bound, and immutable thereafter: there is no
setter and no way to re-stamp a generation. A source that wants to report a new
value publishes new evidence with a new evidence generation; the old record
stays exactly as it was, which is what makes stale-authority rejection and
provenance preservation possible.

Enforced at creation:

* the source identity is well formed and the evidence generation is at least one;
* the declared unit is the dimension's canonical unit;
* `installed` is measured;
* every measured value is non-negative and in the record's unit;
* `observed <= installed` when both are measured;
* `usable + unavailable + residual == installed` exactly whenever all three
  parts are measured;
* `protected + reserved <= usable` whenever all three are measured;
* a source that reports itself `unavailable` has measured **zero** usable
  capacity;
* a source that reports itself `nominal` does not carry an `unavailable` or
  `unknown` state reason;
* provenance is present, valid, and names the same source.

`OperationalState` is `nominal`, `degraded`, `unavailable` or `unknown`. Unknown
is not nominal, and it is not unavailable either.

### 3.5 Reserves and service constraints

A `CapacityReserve` is a **declaration by a named owner** that a quantity of a
dimension is held out of allocation for a reason, over an exact half-open
window. `ReserveKind::protection` contributes to the dimension's protected
capacity; `operational`, `service_obligation` and `contingency` contribute to its
reserved capacity.

Itemisation is reconciled exactly. When a source declares both a roll-up
(`protected` / `reserved`) and itemised reserves for the same dimension, the two
must agree to the unit. When they disagree, the source is **excluded** from the
composition and the dimension is reported `incomplete` with the reason
`reserve_itemisation_mismatch`. Nothing is merged, nothing is averaged, nothing
is silently preferred.

A reserve whose window does not cover the snapshot instant does not reduce
allocatable capacity at all — and, because it is not in force, it also does not
participate in the itemisation reconciliation.

A `ServiceConstraint` is a **floor** that must remain allocatable in a dimension
while its window is active. Each declared constraint is evaluated and reported as
`satisfied`, `violated`, `indeterminate` or `inactive`. The model evaluates the
floor; it does not enforce, admit or schedule anything.

A `CapacityRequirement` is the coverage contract: which dimensions the aggregate
model must answer, and which sources must be present and current for that answer
to be complete. Without that declaration "incomplete" would have no definition.

### 3.6 Outcome precedence

The classification of one dimension's answer is fixed and tested. The first rule
that applies wins, in this order:

1. **`unavailable`** — no evidence covers the dimension and it is not declared
   required: the facility does not offer it.
2. **`incomplete`** — required coverage is missing, or a source was excluded as
   stale, superseded, epoch-mismatched or itemisation-inconsistent.
3. **`unknown`** — every covering source is present and current, and the composed
   allocatable quantity is unmeasured. Never reported as zero.
4. **`exhausted`** — the composed allocatable quantity is measured and exactly
   zero.
5. **`usable`** — the composed allocatable quantity is measured and positive.

`unknown` and `incomplete` are different statements. `incomplete` means *we do
not have everything we asked for*. `unknown` means *we have everything we asked
for and the source did not measure it*.

### 3.7 Limiting-constraint attribution

Dimensions have different units and cannot be compared by magnitude, so the
binding dimension is chosen by the **exact integer permille headroom**
`allocatable * 1000 / usable`, computed with checked 64-bit arithmetic and ties
broken by dimension ordinal. Within a dimension, the dominant reduction is
attributed to one of `limiting_by_unavailable`, `limiting_by_residual`,
`limiting_by_protection` or `limiting_by_reserve`, with the exact amount and the
source that contributes most to it. When the allocatable quantity is unmeasured
the attribution is `limiting_indeterminate` rather than a guess.

## 4. Authority, state and determinism

### 4.1 Typed identities and generations

Identity families are distinct types with no cross-family conversion and no
conversion from a raw integer or a bare string: `FacilityId`, `SiteId`,
`CapacitySourceId`, `ReserveId`, `ConstraintId`, `SnapshotId`, `StoreId`,
`ControllerId`, `ServiceClassId`, `OwnerId`.

Identifiers are validated against a fixed alphabet — ASCII letters, digits, `.`,
`_`, `:`, `-`, beginning with a letter or digit, at most 63 bytes. Refusing path
separators, whitespace, control characters, non-ASCII bytes and the two
relative-path names removes an entire class of path-manipulation and
encoding-confusion input before it reaches any other layer.

Generation families are distinct types too: `CapacityGeneration`,
`EvidenceGeneration`, `Revision`, `EpochId`, `IncarnationId`, `AttemptId`, and
`Tick` (an exact nanosecond instant on the injected clock's timeline). Advancing
a counter is checked: a counter that would wrap is reported as `limit_exceeded`
rather than returning to zero.

### 4.2 Preconditions and validation precedence

Every mutation that depends on current state carries an explicit
`CapacityPrecondition`: expected capacity generation, expected revision, expected
epoch, expected incarnation, and a bounded idempotency `AttemptId`. There is no
wildcard and no "force" flag. The validation precedence is fixed and tested:

```
epoch  ->  incarnation  ->  capacity generation  ->  revision
```

Authority is checked before generation, and generation before revision, so a
caller that is stale in several ways is told the most consequential truth first.

Stale authority is **refused, not merged**:

* a precondition whose epoch or incarnation has moved on fails with
  `stale_authority`, carrying the expected and actual values;
* a precondition whose generation or revision has moved on fails with
  `stale_generation`;
* evidence produced under a different control-plane epoch is refused with
  `stale_authority`;
* evidence whose generation is not strictly newer than the generation already
  held is refused with `stale_generation`;
* a reserve or constraint declaration older than the one already held is refused
  with `stale_generation`.

Idempotency is **bounded and explicit**. The model records the most recent
`limits::max_recorded_attempts` (64) non-zero attempt tokens. Re-submitting a
recorded token succeeds without applying anything a second time. An attempt
token older than the window is no longer recognised as a replay — the documented
bound — and the ordinary precondition check then applies.

### 4.3 The immutable answer

A published `CapacitySnapshot` is immutable, content-addressed
(`shared_ptr<const CapacitySnapshot>`) and generation-bound. It records the exact
evidence records with their generations, their provenance, their digests, the
declared reserves, the evaluated constraints, the composed totals, the
attribution and the findings — so it can be explained, diffed and revalidated
without re-reading any source.

Publication advances the capacity generation by exactly one and the model
revision by exactly one.

`SnapshotFreshness` is `issued` or `recovered`. Freshness is the reader's
stamp on the answer, not part of the answer: it is deliberately excluded from
the canonical encoding and therefore from the content digest, so the digest of a
recovered answer is exactly the digest that was published and a store can verify
that what it read is what it wrote. What keeps a recovered answer from being
mistaken for a fresh one is the stamp itself — `snapshot->freshness()` reports
`recovered` — together with the `recovered_not_revalidated` finding that
`CapacityStore::recover` returns, and a caller that wants to rely on it must
revalidate it against current evidence first.

### 4.4 Revalidation

`revalidate` compares a snapshot with the model's current state and reports every
finding, plus the most severe status. Severity is monotone and fixed:

```
valid  <  stale  <  incomplete  <  superseded
```

* **valid** — nothing rejects the snapshot;
* **stale** — the evidence it used has been superseded by newer evidence, the
  control-plane epoch or the controller incarnation has changed, the coverage
  contract has changed, or the snapshot's validity window has passed;
* **incomplete** — evidence required by the current coverage contract is absent;
* **superseded** — a newer authoritative capacity generation has already been
  published.

A snapshot for a different facility or site is a `conflict` error, not a
revalidation result: it is not the same object being revalidated.

## 5. Persistence and recovery

### 5.1 Store layout

```
FORMAT            immutable manifest: store identity, facility identity,
                  format version and byte-order marker, creation instant
CURRENT           mutable manifest: control-plane authority (epoch, incarnation,
                  controller), published generation, revision, and the name and
                  snapshot digest of the generation it references
capacity.lock     advisory lock file; byte zero carries the store lock
gen-<n>.fcs       a published generation: fixed binary header plus a canonical
                  body holding the whole authoritative state
staging-*.tmp     a commit that has not been published yet
```

A generation file is a 128-byte binary header followed by a canonical body. Every
multi-byte integer is written byte by byte in explicit little-endian order —
never through a native layout — so the format is portable and a byte-swapped or
foreign-endian file is detectably wrong.

### 5.2 Commit protocol

```
plan -> validate -> reserve generation and attempt -> write staging
     -> flush staging -> verify staging by re-reading and re-deriving
     -> publish the generation file by atomic replacement -> flush the directory
     -> replace CURRENT atomically                        <-- the commit point
     -> flush the directory -> retire staging
```

The commit point is the replacement of `CURRENT`. Before it, a crash leaves the
previous generation authoritative and at most one unreferenced generation file
and one staging file behind. After it, the new generation is authoritative.
There is no window in which a reader observes a mixture of the two, because
`CURRENT` is replaced atomically and the generation file it names was already
complete, flushed and verified.

`begin_commit` / `PendingCommit::publish` / `PendingCommit::abandon` expose the
protocol so a caller — and this product's crash tests — can stop between staging
and publication. Destroying an unpublished commit abandons it.

### 5.3 What recovery does, and what it refuses

Recovery reads `CURRENT`, verifies it, reads the generation file it names,
verifies its header, recomputes the body digest, decodes **one whole
authoritative state** — the model state document and the snapshot document
together — and returns it. It never assembles a state out of more than one
generation, and it never mixes a recovered model with a fresher snapshot.

It refuses, safely and specifically:

| Input | Refusal |
| --- | --- |
| file shorter than the 128-byte header | `truncated_input` |
| wrong magic | `corruption` |
| byte-swapped byte-order marker | `incompatible_version` |
| wrong format version or header size | `incompatible_version` |
| declared body larger than the accepted bound | `limit_exceeded` |
| file shorter than the declared body | `truncated_input` |
| file longer than the declared body | `conflict` |
| body digest mismatch | `checksum_mismatch` |
| non-zero reserved header byte | `corruption` |
| corrupt, truncated, malformed or oversized manifest | see §6 |
| a `file_name` that is not exactly `gen-<digits>.fcs` | `path_rejected` |
| a `CURRENT` naming a different store identity | `conflict` |
| state and header disagreeing about the generation | `conflict` |
| snapshot digest not matching the published digest | `checksum_mismatch` |
| a symbolic link or reparse point where a store file is expected | `path_rejected` |

Persisted dynamic evidence does **not** silently become fresh after restart. A
recovered snapshot is stamped `recovered` and carries the
`recovered_not_revalidated` finding, and its digest differs from the issued one.
Revalidating it against the recovered model is what turns it back into a current
answer.

### 5.4 Canonical encoding and integrity beyond a checksum

State and snapshots are encoded as line-oriented UTF-8 with LF terminators and
ASCII content only, with keys emitted in a fixed order chosen by the encoder —
never by iteration over a hash container. Escaping keeps the document pure ASCII
whatever the input bytes were. The same state therefore produces the same bytes
on every platform, in every locale and in every process.

A decoder:

1. bounds the input before parsing it;
2. verifies the document's self-digest, which covers every byte before the
   `digest=` line;
3. parses the input description, requiring **every** field to be consumed exactly
   once: a missing field, an unknown field and a duplicated field are all
   refused;
4. re-derives the answer from that input;
5. re-encodes it and compares the result **byte for byte** with the bytes it was
   given.

Step 5 is what makes the check stronger than a checksum. A snapshot document
carries a `derived_digest` binding the derived answer to the input; editing a
derived value forces the attacker to recompute that digest, and the recomputed
value will not agree with the answer the input actually implies. Editing the
input forces a recomputation of the outer digest, which the decoder verifies.
A document whose derived fields were edited is refused even when its outer digest
was recomputed to match.

The digest is an **integrity** check, not an authenticity mechanism. It detects
corruption and accidental substitution; it is not a signature, and it does not
defend against a writer with write access to the store directory who recomputes
digests deliberately.

## 6. Error model

Every failure is a stable `ErrorCode`, paired with a bounded message and, where
the failure is a comparison, the exact violated constraint or the expected and
actual generations.

| Code | Meaning |
| --- | --- |
| `invalid_argument` | an argument violates a documented precondition |
| `stale_generation` | a generation-bearing token was older than the state |
| `stale_authority` | epoch or controller incarnation authority was superseded |
| `conflict` | the request contradicts committed state |
| `not_found` | a referenced object does not exist |
| `already_exists` | an object that must be unique already exists |
| `incompatible_version` | the version or byte order is not supported |
| `corruption` | bytes are structurally invalid or internally inconsistent |
| `limit_exceeded` | a configured or hard bound would be exceeded |
| `unsupported` | well formed but not implemented by this product |
| `unavailable` | the resource cannot serve the request right now |
| `indeterminate` | the answer is not known and is not zero |
| `permission_denied` | the operating system refused access |
| `io_failure` | an operating-system input/output call failed |
| `lock_conflict` | another process or thread holds the required lock |
| `invariant_violation` | an internal consistency rule would be violated |
| `unit_mismatch` | quantities with different units were combined |
| `duplicate_identity` | the same identity was declared twice |
| `precondition_failed` | a documented precondition was not satisfied |
| `checksum_mismatch` | stored bytes do not hash to the recorded digest |
| `truncated_input` | the input ended before the required length |
| `path_rejected` | a path or path component was rejected as unsafe |
| `account_mismatch` | capacity accounting did not close exactly |
| `not_measured` | the source never measured the requested value |

Reason codes are a separate, equally stable vocabulary: `source_stale`,
`source_superseded`, `source_epoch_mismatch`, `source_incarnation_mismatch`,
`source_state_unknown`, `source_degraded`, `source_unavailable`,
`quantity_not_measured`, `no_source_for_dimension`, `source_not_reported`,
`reserve_itemisation_mismatch`, `allocation_over_committed`,
`source_closure_violated`, `service_floor_violated`,
`service_floor_indeterminate`, `snapshot_expired`,
`capacity_generation_advanced`, `coverage_contract_changed`,
`recovered_not_revalidated`, and the attribution and outcome codes. Both
vocabularies round-trip through their name functions, and an unknown name is
refused rather than mapped to a default.

## 7. Concurrency and process authority

### 7.1 Lock order

The model owns one `std::shared_mutex` protecting its state. Readers take it
shared; writers take it exclusive for the duration of one mutation. **No lock is
ever taken while another is held**: the model never calls into the store, never
calls into the clock and never invokes a callback while holding its own lock. The
clock is sampled *before* the lock is acquired. The lock is never upgraded, there
is no read-modify-write path that releases and re-acquires, and no code path
takes the same lock twice. Published snapshots are immutable values that outlive
the lock and can be handed to any thread.

The store acquires, in this order and never in reverse:

1. the store's advisory **file lock** (`capacity.lock`, byte zero);
2. the store's in-process **state mutex**, which guards only the cached authority
   record.

The state mutex is held for a few instructions and never across a file
operation, a clock call or a caller callback. The file lock is taken once per
operation, is never upgraded from shared to exclusive, and is released before the
operation returns — except across `begin_commit`, where the exclusive lock is
deliberately held by the pending commit until it publishes or is abandoned.

### 7.2 Real process authority

Locking is an operating-system advisory lock on a real file — `LockFileEx` on
Windows, `fcntl` record locking on POSIX — not an in-process mutex. It is proven
with independent operating-system processes in `tests/test_multiprocess.cpp`:

* a second process cannot open the store as a writer while another process holds
  the exclusive lock, and the open fails with `lock_conflict`;
* a reader cannot open while a writer holds the exclusive lock;
* a process that is killed relinquishes the lock, and the next open succeeds;
* a commit made by one process **fences** a commit attempted by another process
  under a superseded epoch, incarnation or generation, with `stale_authority` or
  `stale_generation` reported by the losing process;
* a process that dies between staging and publication leaves the previously
  committed generation authoritative, with the staging file reported as residue;
* a process that dies immediately after a commit returns leaves the committed
  generation durable and recoverable.

Thread-level exclusion within one process is additionally provided by the
model's own mutex. On POSIX, `fcntl` record locks are per-process, so two handles
in the *same* process are not mutually excluded by the file lock alone; the
documented guarantee there is the model mutex and per-operation store locking,
not intra-process file-lock exclusion.

## 8. Testing and proof

The suite is one executable so that the multiprocess tests can re-enter the same
binary as a child process. **No test carries a timeout of any kind** — no CTest
`TIMEOUT`, no shell wrapper, no watchdog, no process time limit, no forced
termination classified as a pass. A hang is a defect to diagnose and fix.

| Suite | What it establishes |
| --- | --- |
| `units`, `measured`, `dimension` | exact integer arithmetic, unit mismatch refusal, unknown-is-not-zero |
| `identity`, `generation`, `provenance` | identifier alphabet, no cross-family conversion, checked counters, window semantics |
| `digest` | SHA-256 against the published FIPS 180-4 vectors, including the one-million-character vector, and incremental/one-shot agreement |
| `evidence` | every rule `SourceEvidence::create` enforces, including exact closure |
| `reserve`, `constraint`, `reason` | declaration validation, window semantics, digest stability, complete name round-trips |
| `derivation` | outcome precedence, exact closure, limiting attribution, completeness |
| `model` | preconditions, validation precedence, bounded idempotency, epoch rules, canonical round trip |
| `revalidate` | every rejection class and its severity ordering |
| `canonical` | determinism across input orderings, round trip, tamper refusal, escaping |
| `diff`, `explain` | exact diffs, pure deterministic ASCII renderings |
| `store`, `store_format` | manifests, fencing, verify, prune, residue, corrupt and swapped state |
| `recovery` | close/reopen, one whole authoritative state, header rejection paths, no residue |
| `property` | conservation over randomized inputs against an independent reference model |
| `adversarial` | malformed and hostile input, path attacks, oversized declarations, overflow |
| `concurrency` | shared/exclusive discipline under threads |
| `multiprocess` | real processes, real locks, real fencing, real crash points |
| `cli` | the built tool driven as a process through a full lifecycle |

### 8.1 Property tests

The central claim — that capacity is conserved and cannot be created from
nowhere — is tested by building random facilities and comparing the published
answer with an **independent reference model** that recomputes the totals from
the raw inputs with plain loops and plain integers, without calling the library's
composition code or using its accounting helpers. Agreement is therefore
evidence rather than tautology.

Asserted over randomized inputs: composed totals equal the reference exactly;
`allocatable <= usable <= installed`; both closure identities hold on the
published answer; a change that only ever reduces installed or usable capacity,
or increases unavailable, protected or reserved capacity, never increases
allocatable; withholding any part of the allocation identity produces `unknown`
and never zero; and shuffling the input order produces byte-identical encodings
and identical digests.

### 8.2 What is not claimed

* **No hardware proof.** Nothing in this repository talks to a PDU, UPS,
  generator, BMS, chiller, switch, NIC or accelerator. Domain associations are
  opaque references. There is no real facility, power, cooling or accelerator
  measurement anywhere in this work.
* **All benchmark and test data is synthetic.** The values are generated
  locally; they are not measurements of any real facility.
* **The POSIX file-locking, flush and rename path is implemented but was not
  exercised on this host.** Validation ran on Windows/MSVC. The POSIX branch
  uses `fcntl`, `fsync` and `rename` and is compiled only on non-Windows targets;
  it is unverified here.
* **Sanitizer coverage is recorded in §10**, including any exact limitation.
* **A store directory is trusted-local.** The *contents* of a store are
  untrusted and are fully validated, but a process with write access to the store
  directory can delete or replace files. The model defends against corruption,
  stale writers and accidental substitution, not against a hostile writer with
  filesystem access who recomputes digests.
* **The clock is injected.** The library never reads a wall clock for an
  authority decision. Expiry is evaluated against a tick the caller supplied.
* **`CapacityStore::authority()` returns a reference** to the cached authority
  and is not synchronised against a concurrent commit from another thread. The
  store's concurrency guarantee is per operation; a caller sharing one handle
  between threads should call `refresh_authority` itself.
* **No reservation, admission or reconciliation lifecycle** is implemented here,
  by design. Declared reserves are evidence.

## 9. Build, install and use

### 9.1 Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Options: `FCAP_BUILD_LIBRARY`, `FCAP_BUILD_TOOLS`, `FCAP_BUILD_TESTS`,
`FCAP_BUILD_EXAMPLES`, `FCAP_BUILD_BENCHMARKS`, `FCAP_WARNINGS_AS_ERRORS`
(default `ON`), `FCAP_ENABLE_ASAN` (default `OFF`).

The library builds warning-clean under MSVC `/W4 /WX /permissive-`; warnings are
never globally suppressed, and the warning interface is a `BUILD_INTERFACE` so
consumers of the installed package never inherit first-party build flags.

### 9.2 Install and consume

```
cmake --install build --prefix /some/prefix
```

```cmake
find_package(facility_capacity 1.0 REQUIRED)
target_link_libraries(your_target PRIVATE dccp::facility_capacity)
```

The package exports the versioned target `dccp::facility_capacity`, the headers
under `include/dccp/facility_capacity/`, and `LICENSE`, `NOTICE` and `README.md`
as documentation. `tests/downstream/` is an independent out-of-tree project that
resolves the package through `find_package` and runs a real minimal lifecycle
against it.

### 9.3 Command-line tool

`fcap` is an inspection and administration tool. It opens stores read-only for
every read command and as a writer for every mutating one.

```
fcap version
fcap store init    --path DIR --facility ID --site ID --epoch N --incarnation N --controller ID [--require DIM]...
fcap store info    --path DIR
fcap store verify  --path DIR [--deep]
fcap store generations --path DIR
fcap store recover --path DIR
fcap store prune   --path DIR --keep N
fcap evidence declare --path DIR --source S --dimension D --generation N [--installed N] ... [--derive-residual]
fcap reserve declare  --path DIR --id ID --source S --dimension D --kind KIND [--amount N] [--window-start N] [--window-end N]
fcap constraint declare --path DIR --id ID --dimension D [--floor N] [--window-start N] [--window-end N]
fcap snapshot show    --path DIR
fcap snapshot explain --path DIR --dimension D
fcap query            --path DIR --dimension D
fcap revalidate       --path DIR
```

Exit status is `0` on success, `1` on a domain failure, `2` on a usage error. A
domain failure prints exactly one line to standard error, of the form
`error: <error_code_name>: <message>`, including the violated constraint and the
expected and actual generations when the error carries them.

### 9.4 Examples

* `examples/quick_start.cpp` — build a model, declare evidence, declare a
  reserve and a service floor, publish, query, mutate a source, publish again and
  diff the two answers.
* `examples/durable_lifecycle.cpp` — create a store, publish, commit, **destroy
  the objects**, reopen, recover, revalidate, verify and prune.

### 9.5 Library sketch

```cpp
ManualClock clock(Tick::from_value(1000));           // time is injected

ModelConfig config;
config.facility = FacilityId::parse("facility-a").value();
config.site     = SiteId::parse("site-1").value();
config.epoch       = EpochId::from_value(7);
config.incarnation = IncarnationId::from_value(3);
auto model = FacilityCapacityModel::create(config, clock).value();

CapacityPrecondition precondition = model.current_precondition();
precondition.attempt = AttemptId::from_value(1);
model.declare_evidence(evidence, precondition);      // typed, immutable, digest-bound

precondition = model.current_precondition();
precondition.attempt = AttemptId::from_value(2);
auto snapshot = model.publish(precondition).value(); // one generation-bound answer
std::puts(snapshot->describe().c_str());

auto answer = model.query(CapacityDimension::power).value();
if (answer.outcome == CapacityOutcome::unknown) {
  // not zero, and not unavailable: the source never measured it
}
```

## 10. Validation performed

Platform: Windows, MSVC 19.44.35207, x64, CMake 4.3.2, Ninja 1.13.2.

Recorded in `VALIDATION.md` alongside this file: the exact commands, the Release
and Debug results, the warning status, the sanitizer result or its exact
limitation, the installed-package and downstream-consumer result, the
fresh-clone result, and the benchmark methodology and measurements.

## 11. Documentation map

| Document | Contents |
| --- | --- |
| `README.md` | this file: boundary, architecture, semantics, persistence, concurrency, error model, build and install, validation summary, limitations |
| `VALIDATION.md` | the exact validation record: commands, results, benchmark methodology and measurements |
| `CONTRIBUTING.md` | licensing of contributions, scope rules, build and test expectations |
| `LICENSE` | Apache License 2.0 |
| `NOTICE` | copyright and third-party notice |
| `examples/` | the public API in use |
| `tests/downstream/` | independent consumer of the installed package |

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
