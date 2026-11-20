// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
// ---- Extent map: the iOS disk as a pinned file inside Android's userdata (OnePlus 12R) ---------------------------------------
// Instead of a GPT partition "unixnu", the iOS root, Data store and log live in one 32 GiB file that Android created and
// pinned in its f2fs userdata (12r/android/unixnu-pin.c). f2fs never moves a pinned file's blocks. The file's physical
// extents were read with FIEMAP and compiled into ExtentTable.h. This layer presents those scattered extents as one
// contiguous *virtual* partition at LBA kExtentVirtualBase, far above any real LBA of the 12R's LUN 0 (61332480 blocks).
//
// Every 4096-byte LBA the rest of the driver uses is either
//   - below kExtentVirtualBase: a real LBA, READ only (GPT checks); every write there is refused, or
//   - inside [kExtentVirtualBase, kExtentVirtualBase + kExtentTotalBlocks): translated through the table, read and write
//     (writes still pass the store/log window checks of RootCommands.h first, in virtual LBAs).
// Anything else is refused. Translated physical LBAs are, by construction, inside the pinned file and inside userdata.
//
// The table is valid only for the exact userdata partition it was made from: findRootLayout() refuses extent mode unless the
// real GPT entry "userdata" has exactly kExtentHostFirst..kExtentHostLast. Recreating the file or repartitioning needs a new
// table (and a new kernel); a stale table is refused at boot, never used.
#include <stdint.h>

#ifndef kIOReturnNotPermitted
#define kIOReturnNotPermitted ((IOReturn) 0xe00002e2)   // IOKit's value; for host fixtures without the full error list
#endif

#if defined(SHENG_UFS_EXTENT_MAP) && SHENG_UFS_EXTENT_MAP
#include "ExtentTable.h"   // generated: kExtentCount, kExtents[], kExtentTotalBlocks, kExtentHostFirst/Last, kExtentTableSha256
#else
struct UfsExtent { uint32_t vfirst; uint32_t pfirst; uint32_t blocks; };
static constexpr uint32_t kExtentCount = 0;
static constexpr UfsExtent kExtents[1] = { { 0, 0, 0 } };
static constexpr uint64_t kExtentTotalBlocks = 0, kExtentHostFirst = 0, kExtentHostLast = 0;
#endif

static constexpr uint64_t kExtentVirtualBase = 0x80000000ULL;

// 0 = unknown yet, 1 = on, 2 = off. Decided once by extentModeDecide() (boot-arg unixnu-extent=1 and a valid table).
static int gExtentMode = 0;
static const char *gExtentWhy = "not checked";

// Checks the compiled table: virtual extents in order and gap-free from 0, every physical range inside the host
// partition, total equal to kExtentTotalBlocks. Physical overlap is checked by the generator (sorting); a table that
// passed it and this check cannot map two virtual blocks to one physical block unless the generator is wrong.
static inline bool extentTableValid (const char **why)
{
  if (!kExtentCount || !kExtentTotalBlocks) { *why = "no extent table compiled in"; return false; }
  if (kExtentHostLast < kExtentHostFirst || kExtentHostLast > 0xffffffffULL) { *why = "extent host range"; return false; }
  if (kExtentVirtualBase <= kExtentHostLast || kExtentTotalBlocks > 0xffffffffULL - kExtentVirtualBase) {
    *why = "extent virtual base"; return false;
  }
  uint64_t expect = 0;
  for (uint32_t i = 0; i < kExtentCount; ++i) {
    const UfsExtent &e = kExtents[i];
    if (e.vfirst != expect || !e.blocks) { *why = "extent virtual order"; return false; }
    if (e.pfirst < kExtentHostFirst || uint64_t (e.pfirst) + e.blocks - 1 > kExtentHostLast) {
      *why = "extent outside userdata"; return false;
    }
    expect += e.blocks;
  }
  if (expect != kExtentTotalBlocks) { *why = "extent total"; return false; }
  return true;
}

static inline bool extentModeOn (void) { return gExtentMode == 1; }

// Decided once, before the first LUN 0 access of the root layout. `requested` is boot-arg unixnu-extent=1.
// Extent mode turns on only with a compiled-in table that passes extentTableValid(); otherwise the driver behaves as
// before (GPT partition "unixnu"), which on the 12R simply finds no root and touches nothing.
static inline void extentModeDecide (bool requested)
{
  if (gExtentMode) return;
  if (!requested) { gExtentMode = 2; gExtentWhy = "off"; return; }
  const char *why = "";
  if (!extentTableValid (&why)) { gExtentMode = 2; gExtentWhy = why; return; }
  gExtentMode = 1;
  gExtentWhy = "on";
}

// Index of the extent holding virtual block `rel` (0-based inside the virtual partition), by binary search.
static inline uint32_t extentFind (uint64_t rel)
{
  uint32_t lo = 0, hi = kExtentCount;
  while (hi - lo > 1) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (kExtents[mid].vfirst <= rel) lo = mid; else hi = mid;
  }
  return lo;
}

// Translates [lba, lba + blocks) of the virtual partition into its first physical run.
// Returns the run length in blocks (1..blocks) and the physical LBA in *physical, or 0 if any part is outside.
static inline uint32_t extentTranslate (uint64_t lba, uint32_t blocks, uint64_t *physical)
{
  if (!extentModeOn () || !blocks || lba < kExtentVirtualBase) return 0;
  uint64_t rel = lba - kExtentVirtualBase;
  if (rel >= kExtentTotalBlocks || blocks > kExtentTotalBlocks - rel) return 0;
  const UfsExtent &e = kExtents[extentFind (rel)];
  uint64_t into = rel - e.vfirst;
  if (into >= e.blocks) return 0;   // cannot happen with a valid table
  uint64_t run = e.blocks - into;
  *physical = uint64_t (e.pfirst) + into;
  return run < blocks ? static_cast<uint32_t> (run) : blocks;
}

// Runs `io(physicalLba, runBlocks, byteOffset)` for each physical run of [lba, lba + blocks). For a real LBA (below the
// virtual base) it runs once with the LBA unchanged when `allowReal`. Returns false before any I/O if the range is refused,
// or with the first failure of io.
template <class Io>
static inline IOReturn extentForEach (uint64_t lba, uint32_t blocks, bool allowReal, Io io)
{
  if (!extentModeOn ()) return io (lba, blocks, 0);
  if (lba < kExtentVirtualBase) {
    if (!allowReal || lba > kExtentVirtualBase - blocks) return kIOReturnNotPermitted;
    return io (lba, blocks, 0);
  }
  // Check the whole range first so that a refused request does no partial I/O.
  uint64_t rel = lba - kExtentVirtualBase;
  if (rel >= kExtentTotalBlocks || blocks > kExtentTotalBlocks - rel) return kIOReturnNotPermitted;
  uint32_t done = 0;
  while (done < blocks) {
    uint64_t physical = 0;
    uint32_t run = extentTranslate (lba + done, blocks - done, &physical);
    if (!run) return kIOReturnNotPermitted;
    IOReturn status = io (physical, run, uint64_t (done) * 4096);
    if (status != kIOReturnSuccess) return status;
    done += run;
  }
  return kIOReturnSuccess;
}
