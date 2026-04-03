# Unit Test Sweep

This file tracks what the standalone C++ harness in `contract_qugate.cpp` does and does not cover.

## Purpose

- make current coverage explicit
- prevent "we have lots of tests" from hiding major feature gaps
- separate harness-supported tests from features that require a richer harness or live node

## Current Harness Reality

The standalone harness is still a partial model of `QuGate.h`.

It currently covers well:

- SPLIT
- ROUND_ROBIN
- THRESHOLD
- CONDITIONAL
- fee escalation / dust / expiry / free-list basics
- chain routing basics

It does not yet model the live contract in full for:

- HEARTBEAT
- MULTISIG
- TIME_LOCK
- admin-gate governance
- gate-as-recipient deferred routing
- versioned gate IDs end-to-end
- latest execution metadata query

## Sweep Plan

### 1. Invariant tests

These should be added aggressively because they catch regressions across many modes:

- `totalReceived`
- `totalForwarded`
- `currentBalance`
- reserves
- free-list and generation reuse
- transfer-first behavior

### 2. Mode matrix

Each supported mode should have:

- happy path
- boundary values
- invalid config
- inactive/closed behavior
- chain interaction where relevant

### 3. Regression tests

Every real bug found should get a direct regression test where the harness can represent it.

### 4. Harness gaps

Before claiming full contract coverage, the harness needs upgrades for:

- modes 6/7/8
- admin gate rules
- gate-as-recipient deferred routing
- query surface parity with the live contract

## Immediate Next Targets

- expand RANDOM and ROUND_ROBIN coverage
- add more chain invariants
- add lifecycle invariants around slot reuse and rollback
- add tests for latest execution metadata once the harness models it
