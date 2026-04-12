# ThomasFok / eositis MegaFlash — upstream diff findings

**Purpose:** Side-by-side reference while you work in the repo. Compares **this fork (`eositis`)** `main` to **[ThomasFok/MegaFlash](https://github.com/ThomasFok/MegaFlash)** `main`, with emphasis on **storage** and **Apple IIc Plus / Applesoft acceleration**.

**Refresh the diff locally:**

```bash
cd /path/to/MegaFlash
git fetch upstream main
git log --oneline main..upstream/main | head -50
git diff main upstream/main --stat -- firmware/ pico/ramdisk.c pico/romdisk.c pico/flash.c pico/dmamemops.c pico/slinky.c
```

**Remote:** `upstream` → `https://github.com/ThomasFok/MegaFlash.git` (see `git remote -v`).

**Context:** Your tree adds **NetworkPump**, **Uthernet II**, extended TFTP, build tooling, etc. Thomas’s line is roughly **V1.1.9 / V1.1.10** on GitHub. Merges are **non-trivial** — prefer cherry-picks or manual ports with **IIc / IIc+** hardware checks.

---

## Quick reference (high value)

| Topic | Your `main` vs Thomas `main` | Action |
|--------|--------------------------------|--------|
| **`firmware/accel.s`** | **Identical** in sampled diff | No port needed for accelerator *module* text |
| **IIc+ boot / bank switch** | Thomas adds **`fswrts`**, **`swjmp_ay`**, **`$FB19` → `jmp slxeq`**, boot menu via stack+`fswrts` | **Ported in this fork** (§4h): `patches.s`, `megaflash.s`, `B1_FFC8`, **`merge_iic*`** — still **hardware-test on ZIP** |
| **Applesoft patches** | Same three fixes; Thomas reworks hook around **`fswrts`** | Merge with **`fswrts`** work |
| **RAM disk** | Thomas: **dedicated DMA channel**, mutex on exports only | Consider if TFTP + RAM disk + dual-core stress |
| **ROM disk** | Thomas: **no DMA** in `romdisk.c` | Compare behaviour vs your DMA path |
| **Flash / DMA** | Large refactors; fix **`ReadFromFlashByDMA` hang when overclocked** (`3bfb309`) | **Cherry-pick** if overclocking |
| **TFTP / images** | Thomas: **DOS-order download** + **`imagewriter`** (`52cc99e`), **`blockCount<8`** guard (`40ac448`) | Feature port; integrate with **NetworkPump** carefully |
| **SmartPort / megaflash.s** | Substantial ROM layout + transfer churn | Review **with** `fswrts` port |

---

## 1. Apple IIc Plus acceleration & Applesoft (firmware)

### 1.1 `firmware/accel.s`

**No textual diff** between `main` and `upstream/main` — ZIP-style accelerator commands and registers match.

### 1.2 `fswrts` + bank-switch / boot path (Thomas-side)

Thomas adds **`fswrts`** in **`firmware/patches.s`** and uses it from **`firmware/megaflash.s`**. Commit message theme: *“Added fswrts routine to improve performance on IIc+”*.

Mechanism (from upstream comments / diff):

- **Bank switch** instruction placed so execution continues with **`RTS`** on **bank 0** at **`$FFCB`**, improving behaviour with **IIc Plus / ZIP** caching (**`$C000–$CFFF`** not cached like main RAM).
- **`$FB19` cold-start patch:** **`jmp slxeq`** instead of a **`SLOTROM`** helper that **`jsr slxeq`** then **`jmp ($0)`**.
- **Boot menu:** **`swjmp_ay` / `swjmp_ay_sp0`** — push **target−1**, **`jmp fswrts`**, **RTS** to bank 0 — instead of **`jmp BMRUN`** from aux ROM.
- **No boot menu:** return to **`jmp ($0000)`** via **stack + `fswrts`**.

**Why it matters:** Correctness and/or speed when ROM and driver span **banks** and the **accelerator cache** matters. **Validate on real IIc Plus** after any port.

### 1.3 `firmware/patches.s`

Beyond **`fswrts`** / **`$FB19`**, the **Applesoft** patch regions (ONERR, PRINT speed, integer **−32768**) are the **same ideas**; upstream adds comments and structure.

### 1.4 `firmware/megaflash.s`, `smartport.s`, `slotrom.s`

Thomas changes **`megaflash.s`** heavily: e.g. **`HOMESEGMENT`**, **`swjmp_ay`**, debug print segment, **`getdib` segment move (historical commit). **`smartport.s`** has non-trivial edits. Treat as **one package** with **`fswrts`**.

---

## 2. Storage stack (Pico)

### 2.1 `pico/ramdisk.c`

- Dedicated **DMA channel** for RAM disk copies/zero (not only shared **`dmamemops`**).
- Mutex: protect **exported** entry points that touch RAM disk data.

Example upstream commits: *“Ramdisk has its own dedicated DMA channel”*, *“Mutex Lock in exported functions only”*.

### 2.2 `pico/romdisk.c`

Upstream **removed DMA** from ROM disk handling. Compare if you want the same simplicity.

### 2.3 `pico/flash.c` / `pico/dmamemops.c`

Large diffs: refactors, DMA loop optimizations, and:

| Commit | Topic |
|--------|--------|
| **`3bfb309`** | **Fix: `ReadFromFlashByDMA()` hangs when CPU is overclocked** |

### 2.4 `pico/slinky.c`

Mostly cleanup in the diff stat; skim if debugging Slinky timing.

---

## 3. TFTP / image storage (network)

| Commit | Topic |
|--------|--------|
| **`52cc99e`** | **TFTP Download with DOS order image support** — adds **`imagewriter`** and TFTP path changes |
| **`40ac448`** | **Discarded the data if blockCount<8** — guard in **`imagewriter.cpp`** |

Your fork’s **NetworkPump** / TFTP stack differs; treat as a **feature design** merge, not a blind cherry-pick.

---

## 4. Lower priority upstream deltas

- **`3be8cd2`** — device info shows **GCC version** (diagnostics).
- **C++ style / structure** — `unique_ptr`, `override`, interrupt helper renames, UDP callback naming. Conflicts with your **`network_pump`** architecture unless you drop that layer.

---

## 5. Suggested order of work

1. **IIc+ ROM path:** Port or cherry-pick **`fswrts` / `swjmp_*`** + matching **`megaflash.s` / `patches.s`**; test **cold start** and **boot menu** on **IIc Plus**.
2. **Overclock:** Apply **`3bfb309`** (or equivalent) if you raise **SYS_CLK** and see flash DMA stalls.
3. **RAM disk:** Port dedicated DMA + mutex policy if you stress **TFTP + RAM disk** on **both cores**.
4. **DOS-order TFTP:** Only if you need Thomas’s **`imagewriter`** flow — plan integration with **NetworkPump**.

---

## 6. Duplicate doc location

A copy lived under **`docs/Upstream-ThomasFok-storage-accel-comparison.md`**; this **root** file is the one to keep open beside your editor. **`docs/Implementation-notes-and-reasoning.md`** §4d and the summary table point here.

---

*Snapshot vs `upstream/main` at time of analysis; re-run `git fetch` + `git diff` after either branch moves.*
