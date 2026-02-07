# Mirror Mod

A [Totems protocol](https://github.com/totemprotocol) mod that creates **mirror tokens** — derivative tokens backed 1:1 by a base totem. Only the totem creator can mint mirrors by depositing base tokens. Anyone can redeem mirrors back to base tokens, which burns the mirrors.

One base totem can back multiple different mirror totems.

## How It Works

### Overview

```
BASE token (real)  ──deposit──>  Mirror Contract  ──mint──>  SYNTH token (mirror)
SYNTH token (mirror)  ──redeem──>  Mirror Contract  ──return──>  BASE token (real)
                                                    ──burn──>   SYNTH destroyed
```

Every mirror token in circulation is fully backed by a locked base token in the contract. The ratio is always 1:1.

### Setup

Before minting, the creator must link a mirror totem to a base totem:

1. **Create a base totem** (e.g. `4,BASE`) with tokens allocated to the creator
2. **Publish the mirror mod** on the Totems marketplace with Transfer + Mint hooks
3. **Create a mirror totem** (e.g. `4,SYNTH`) with:
   - The mirror contract as the minter (`is_minter: true`)
   - `mint` and `transfer` hooks pointing to the mirror contract
4. **Call `mirror::setup('4,SYNTH', '4,BASE')`** to link the two tokens

Requirements enforced by `setup`:
- Both totems must exist
- Both must have the same creator
- Both must have the same decimal precision
- They must be different tokens

### Minting (Creator Only)

Minting is a two-step process:

```
Step 1:  totems::transfer(creator, mirror_contract, "100.0000 BASE", "")
Step 2:  totems::mint(mirror_contract, creator, "0.0000 SYNTH", "0.0000 A", "")
```

**Step 1** deposits base tokens into the mirror contract. Nothing happens yet — the tokens just sit there.

**Step 2** triggers the mint. The totems contract calls `mirror::mint` as an inline action. The mirror contract:

1. Verifies the caller is the totems contract (`get_sender()` check)
2. Verifies the minter is the totem's creator
3. Calculates the **deposit delta**: sums all `base_locked` for the same base ticker across all pairings, compares against the contract's actual base token balance
4. Mints mirror tokens equal to the delta and sends them to the creator
5. Updates `base_locked` to track the new reserves

The `quantity` parameter in the mint call is ignored — the contract determines how many mirrors to mint based on how many base tokens were deposited.

**Why two steps?** The mirror contract can't intercept base token transfers as a mint trigger because it's only registered on the mirror totem's hooks, not the base totem's hooks. The deposit and mint are linked by the deposit delta pattern.

### Redemption (Anyone)

Redemption is a single step:

```
totems::transfer(user, mirror_contract, "50.0000 SYNTH", "")
```

When mirror tokens are sent to the contract, the `on_transfer` handler:

1. Looks up the pairing for the incoming token
2. Verifies sufficient base reserves (`base_locked >= amount`)
3. Decreases `base_locked`
4. Sends equivalent base tokens to the redeemer
5. Burns the mirror tokens via an inline action to the totems contract

All of this happens atomically in a single transaction. If any step fails, the entire transaction reverts.

### Multiple Mirrors Per Base

One base token can back multiple mirror tokens (e.g. SYNTH and SYNTH2 both backed by BASE). Each pairing tracks its own `base_locked` independently.

When minting, the delta calculation sums `base_locked` across **all** pairings for that base ticker. This prevents the creator from double-counting a single deposit across multiple mirrors.

```
State: SYNTH locked=100 BASE, SYNTH2 locked=50 BASE, actual balance=150 BASE

Creator deposits 75 BASE (balance becomes 225)
Creator mints SYNTH:
  total_tracked = 100 + 50 = 150
  delta = 225 - 150 = 75
  SYNTH locked becomes 175, SYNTH2 unchanged at 50
```

### Security

| Check | Where | Purpose |
|-------|-------|---------|
| `get_sender() == TOTEMS_CONTRACT` | `mint` | Only totems contract can call mint |
| `minter == synth_totem.creator` | `mint` | Only creator can mint mirrors |
| `base_totem.creator == synth_totem.creator` | `setup` | Both totems must share a creator |
| `base_locked >= quantity` | `on_transfer` | Can't redeem more than reserves |
| `check_license` | `mint`, `on_transfer` | Contract must be licensed for the totem |
| `delta > 0` | `mint` | Can't mint without depositing |

**Invariant:** The sum of all `base_locked` across pairings for a given base ticker always equals the contract's actual base token balance (excluding untracked deposits waiting to be minted).

## Contract Structure

```
contracts/
  mirror/
    mirror.cpp        # The smart contract
  library/
    totems.hpp        # Totems protocol library (dependency)
build/
  mirror.wasm         # Compiled WebAssembly
  mirror.abi          # Contract ABI
tests/
  mirror.spec.ts      # Test suite
```

### Pairings Table

| Field | Type | Description |
|-------|------|-------------|
| `synth_ticker` | `symbol_code` | Mirror token symbol (primary key) |
| `base_ticker` | `symbol_code` | Base token symbol (secondary index) |
| `base_locked` | `asset` | Base tokens locked as reserves |

### Actions

| Action | Parameters | Description |
|--------|-----------|-------------|
| `setup` | `synth_ticker`, `base_ticker` | Link a mirror totem to a base totem |
| `mint` | `mod`, `minter`, `quantity`, `payment`, `memo` | Called by totems contract to mint mirrors |

### Notification Handlers

| Handler | Trigger | Description |
|---------|---------|-------------|
| `on_mint` | `TOTEMS_MINT_NOTIFY` | Empty (required hook) |
| `on_transfer` | `TOTEMS_TRANSFER_NOTIFY` | Handles redemption and accepts base deposits |

## Build

```bash
# Compile
eosio-cpp -abigen -I contracts/library -o build/mirror.wasm contracts/mirror/mirror.cpp
```

## Deploy

```bash
# Set eosio.code permission (required for inline actions)
cleos -u https://jungle4.greymass.com set account permission <mirror_account> active \
  --add-code -p <mirror_account>@active

# Deploy contract
cleos -u https://jungle4.greymass.com set contract <mirror_account> build/ \
  mirror.wasm mirror.abi -p <mirror_account>@active

# Publish on market
cleos -u https://jungle4.greymass.com push action modsmodsmods publish \
  '["<seller>", "<mirror_account>", ["transfer", "mint"], 0,
    {"name": "Mirror", "summary": "1:1 backed mirror tokens",
     "markdown": "", "image": "<image_url>", "website": "",
     "website_token_path": "", "is_minter": true}, [], null]' \
  -p <seller>@active

# Create base totem
cleos -u https://jungle4.greymass.com push action totemstotems create \
  '["<creator>", "4,BASE",
    [{"label": "Creator", "recipient": "<creator>", "quantity": "1000000.0000 BASE", "is_minter": null}],
    {"transfer": [], "mint": [], "burn": [], "open": [], "close": [], "created": []},
    {"name": "Base Token", "description": "", "image": "<image_url>",
     "website": "", "seed": "<random_64_char_hex>"}, null]' \
  -p <creator>@active

# Create mirror totem
cleos -u https://jungle4.greymass.com push action totemstotems create \
  '["<creator>", "4,SYNTH",
    [{"label": "Mirror supply", "recipient": "<mirror_account>", "quantity": "1000000.0000 SYNTH", "is_minter": true}],
    {"transfer": ["<mirror_account>"], "mint": ["<mirror_account>"], "burn": [], "open": [], "close": [], "created": []},
    {"name": "Synth Token", "description": "", "image": "<image_url>",
     "website": "", "seed": "<random_64_char_hex>"}, null]' \
  -p <creator>@active

# Setup pairing
cleos -u https://jungle4.greymass.com push action <mirror_account> setup \
  '["4,SYNTH", "4,BASE"]' -p <creator>@active
```

## Usage

```bash
# Mint: deposit base tokens then trigger mint
cleos -u https://jungle4.greymass.com push action totemstotems transfer \
  '["<creator>", "<mirror_account>", "100.0000 BASE", ""]' -p <creator>@active

cleos -u https://jungle4.greymass.com push action totemstotems mint \
  '["<mirror_account>", "<creator>", "0.0000 SYNTH", "0.0000 A", ""]' -p <creator>@active

# Redeem: send mirror tokens back to get base tokens
cleos -u https://jungle4.greymass.com push action totemstotems transfer \
  '["<user>", "<mirror_account>", "50.0000 SYNTH", ""]' -p <user>@active

# Check pairings
cleos -u https://jungle4.greymass.com get table <mirror_account> <mirror_account> pairings
```

## Jungle4 Testnet

Deployed and tested on Jungle4:

| Account | Role |
|---------|------|
| `mirrormirror` | Mirror mod contract |
| `bespangle123` | Creator / seller |
| `totemstotems` | Totems contract |
| `modsmodsmods` | Market contract |
