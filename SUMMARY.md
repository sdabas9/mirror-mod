# Mirror Mod — Implementation Summary

## What is the Mirror Mod?

A Totems protocol mod that creates **mirror tokens** — synthetic derivative tokens backed 1:1 by a base totem token. Only the totem creator can mint mirror tokens by depositing base tokens. Anyone can redeem mirror tokens back to base tokens, which burns the mirrors. One base totem can back multiple different mirror totems.

## Files Created

- `contracts/mirror/mirror.cpp` — The smart contract (originally named `synthetics`, renamed to `mirror`)
- `tests/mirror.spec.ts` — Comprehensive test suite

## Contract Design

### Data Structure

**Pairings table** — maps each mirror token to its base token:
- `synth_ticker` (primary key) — the mirror token symbol
- `base_ticker` — the base token symbol
- `base_locked` — amount of base tokens locked as reserves
- Secondary index on `base_ticker` for summing total locked per base

### Actions

| Action | Who calls it | Purpose |
|--------|-------------|---------|
| `setup` | Creator (direct) | Links a mirror totem to a base totem |
| `mint` | Totems contract (inline) | Calculates deposit delta, mints mirror tokens |
| `on_mint` | Totems contract (notification) | Empty — required hook handler |
| `on_transfer` | Totems contract (notification) | Redeems mirror tokens for base tokens |

### Minting Flow (creator only)

1. Creator transfers base tokens to the mirror contract
2. Creator calls `totems::mint` with the mirror mod
3. Mirror contract calculates delta (actual balance - total tracked reserves)
4. Mints equivalent mirror tokens to the creator
5. Updates `base_locked`

### Redemption Flow (anyone)

1. User transfers mirror tokens to the mirror contract
2. Mirror contract sends equivalent base tokens to the user
3. Mirror contract burns the mirror tokens (inline action)
4. Decrements `base_locked`

### Multiple Mirrors Per Base

When minting, the delta is calculated by summing `base_locked` across ALL pairings for the same base ticker, then comparing to the actual balance. This prevents double-counting a single deposit across multiple mirror tokens.

## Bug Fix During Deployment

The original `on_transfer` handler called `check_license` before checking if the incoming token was a mirror token. This caused BASE token deposits to fail because the mirror contract is only licensed for SYNTH, not BASE.

**Fix:** Moved the guard (`to != get_self()`) and pairing lookup before the license check. If the incoming token isn't a known mirror token (e.g. it's a base token deposit), the handler returns silently.

## Deployment on Jungle4

| Account | Role |
|---------|------|
| `mirrormirror` | Mirror mod contract |
| `bespangle123` | Creator / seller |
| `totemstotems` | Totems contract |
| `modsmodsmods` | Market contract |

### Steps Executed

1. Set `eosio.code` permission on `mirrormirror`
2. Bought RAM for contract deployment
3. Deployed `mirror.wasm` and `mirror.abi` to `mirrormirror`
4. Published mod on market (Transfer + Mint hooks, is_minter: true)
5. Deposited 200 A tokens to totems contract for creation fees
6. Created `4,BASE` totem — 1,000,000 BASE allocated to `bespangle123`
7. Created `4,SYNTH` totem — 1,000,000 SYNTH allocated to `mirrormirror` as minter
8. License auto-created for `mirrormirror` on SYNTH
9. Called `mirror::setup('4,SYNTH', '4,BASE')` to link the pairing

### Test Results

| Test | Result |
|------|--------|
| Deposit 100 BASE + mint SYNTH | 100 SYNTH minted, `base_locked = 100` |
| Redeem 50 SYNTH | 50 BASE returned, 50 SYNTH burned, `base_locked = 50` |

### Final State

```
bespangle123: 999,950.0000 BASE, 50.0000 SYNTH
mirrormirror pairings: SYNTH → BASE, base_locked = 50.0000 BASE
```

## Build

```bash
# Compile (using eosio-cpp v1.8.1)
eosio-cpp -abigen -I contracts/library -o build/mirror.wasm contracts/mirror/mirror.cpp

# Deploy
cleos -u https://jungle4.greymass.com set contract mirrormirror build/ mirror.wasm mirror.abi -p mirrormirror@active
```

## Note on Test Framework

The WASM compiled by `eosio-cpp` v1.8.1 is incompatible with the local `@vaulta/vert@2.1.1` test framework (produces `Cannot read properties of undefined (reading 'buffer')` errors). The newer `cdt-cpp` (CDT v4.1.x) is needed for local tests. A `Dockerfile.cdt` was created to install CDT from the pre-built .deb package instead of compiling from source. On-chain deployment works fine with the eosio-cpp output.
