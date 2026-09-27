# Contributing to Facility Capacity

We welcome contributions from individuals and organizations.

## Contribution terms

Contributions are submitted under the terms of the Apache License 2.0. No
Contributor License Agreement (CLA) is required. By submitting a contribution
you agree that it may be distributed under the Apache License 2.0.

## Scope and boundaries

Keep changes within the repository's architectural and system boundary. Facility
Capacity owns the *aggregate* facility-capacity model: it composes typed,
immutable, generation-stamped evidence from the space, rack, power, cooling,
operational-reserve and facility-service dimensions into one generation-bound,
published, explainable answer, and it decides when that answer must be rejected
as stale or incomplete.

It does not own the source systems. It is not Rack Capacity, Space Capacity,
Power Capacity, Cooling Capacity, Facility Placement Planner, Facility Capacity
Reservation or Capacity Reconciliation. Their policy, derivation rules,
lifecycle and inventory mutation stay in those repositories; Facility Capacity
consumes their published evidence as immutable typed values and never
reimplements their policy.

It also does not schedule accelerator workloads or reserve accelerator memory
(Accelerated Systems Infrastructure), does not choose network paths or reserve
fabric bandwidth (Distributed Fabric Infrastructure), does not place hardware,
does not actuate electrical or cooling equipment, and does not mutate any source
inventory. Do not expand scope into adjacent systems or sibling repositories.

## Quality expectations

Changes should meet the repository's normal code-quality, build, test,
documentation and cleanup expectations. Run the full build and test suite before
opening a pull request and leave the working tree clean. The project builds
warning-clean with warnings treated as errors; a change that introduces a
first-party warning is not acceptable.

Invariants are enforced in the library, not only in tests. A change that weakens
a documented invariant, a precondition check, an accounting-closure check or an
integrity check needs to say so explicitly in its description and to update the
README.

No test carries a timeout. A hang is a defect to diagnose and fix, never
something to terminate or classify as a pass.

## Attribution

Do not add AI attribution or unintended "Co-authored-by" trailers to commit
messages. The commit author is the authoritative attribution.

## Telemetry

Do not introduce telemetry transmission.
