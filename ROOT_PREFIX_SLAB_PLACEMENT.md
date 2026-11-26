# Root-prefix slab placement

Keys that share a root prefix hash are written to slabs that hold only that root. A root may own any number of slabs, but a slab never holds items of two roots.

Status: implemented but not compiled or run. No folly or fbthrift is installed on the dev machine, so the changes were reviewed by hand only.

## Decisions

| Question | Choice |
|---|---|
| Where the root hash comes from | Passed explicitly to `allocateWithRoot()` |
| Eviction when the pool is full | Strict: only items of the same root are recycled; the allocation fails if the root has none |
| Chained items | Placed in the parent's root; they are not hashed independently |

Hash `0` (`kNoRootHash`) means "no root". Such allocations behave as before and only use slabs that no root owns.

## API

```cpp
WriteHandle CacheAllocator::allocateWithRoot(PoolId id, uint64_t rootHash, Key key,
                                             uint32_t size, uint32_t ttlSecs = 0,
                                             uint32_t creationTime = 0);
```

Lower layers gained an optional trailing `rootHash` argument (default `kNoRootHash`), so existing callers compile unchanged:

- `MemoryAllocator::allocate(pid, size, rootHash)`
- `MemoryPool::allocate(size, rootHash)`
- `AllocationClass::allocate(rootHash)` and `addSlabAndAllocate(slab, rootHash)`
- `MemoryAllocator::getRootHash(memory)` returns the owner of the slab that holds `memory`.

## How it works

### Slab ownership (`Slab.h`)

`SlabHeader` has a new `uint64_t rootHash` field with `getRootHash()` and `setRootHash()`. It is reset by `initialize()`, `resetAllocInfo()` and `clearClassAndUnmark()`, so a released slab is unowned again. The header grows from 7 to 15 bytes.

### Per-root allocation state (`AllocationClass`)

The class keeps its existing state for the no-root case. For roots it adds:

```cpp
struct RootState {
  Slab* currSlab;       // slab this root is carving from
  uint32_t currOffset;
  FreeList freed;       // freed allocations of slabs owned by this root
  uint32_t numSlabs;
  bool hasSpace;
};
std::unordered_map<uint64_t, RootState> roots_;
size_t rootsWithSpace_;
```

- **Allocate.** A root takes from its own freed list, then from its current slab, and otherwise takes a free slab of the class, assigns it to itself and makes it current. A root with no slab can only start from a free slab.
- **Free.** `free()` reads the slab header's root and puts the allocation on that root's list, so only the owner can reuse it.
- **`canAllocate_`.** This fast-path flag is now recomputed from free slabs, the no-root state and `rootsWithSpace_`. Without that, one root running out of room would make the class report it was full to every other root.
- **Slab release.**
  - `startSlabRelease` resets the owning root's current slab, not just the class-wide one.
  - Pruning, and restoring freed allocations after an abort, operate on the owner root's free list.
  - When a root's last slab is released its `RootState` is erased.
- **Stats.** `getStats()` sums freed allocations and uncarved space over all roots.

### Pool (`MemoryPool`)

`allocate(size, rootHash)` passes the root to the allocation class, including when a new slab is added to the class.

### Eviction (`CacheAllocator.h`)

- `allocateInternal`, `findEviction` and `getNextCandidate` take the root.
- An atomic flag `rootedAllocsUsed_` is set by the first `allocateWithRoot()` with a non-zero root. Until then, eviction behaves as before and pays no extra cost.
- After it is set, `getNextCandidate` skips any candidate whose slab root differs from the requesting root.
- If `evictionSearchTries == 0` and the root has nothing to evict, the search gives up after 3 empty passes instead of looping forever.
- Chained items use the parent's root. Items moved during slab rebalancing keep the old item's root.

### Persistence

- `objects.thrift` has a new `AllocationClassRootObject` (root hash, current slab index and offset, freed list) and `AllocationClassObject.roots` (field 13).
- Restore rebuilds each root's `numSlabs` from the slab headers of the allocated slabs and re-poisons uncarved space for ASAN.
- `kCachelibVersion` is 20 and `kCacheRamFormatVersion` is 6 because the slab header layout changed. Existing shared-memory caches will be dropped on upgrade.

## Root slab tier: demote and promote whole slabs through Navy

Status: implemented but not compiled. The std-only parts (`RootHotness`, `RootSlabImage.h`) are unit-tested and pass locally. Everything that needs folly, thrift or Navy (`CacheAllocator`, `NvmCache`, `NvmCacheTests.cpp`) was reviewed by hand only.

Enable with `config.enableRootSlabTier(RootSlabTierConfig{...})`. It needs a Navy cache; without one the tier stays inactive.

### Hotness

`RootHotness` keeps, per root, an access score that halves every `hotnessHalfLifeSecs` (default 600), plus the time of the last access. The score is the primary order; the last access time breaks ties. It is bumped by:

- `allocateWithRoot()` (one per allocation)
- `findWithRoot()` and `findToWriteWithRoot()` (one per call, hit or miss)

Plain `find()` does not know the root and is not counted.

### Demotion (DRAM to Navy)

Triggers:

- **On allocation failure.** A rooted allocation (or chained item) that finds no memory calls `demoteColdestRootSlab(pid, cid, root)` before it falls back to evicting its own items. It only demotes a root strictly colder than the requester, so a hot root never loses a slab to a colder one. This also relieves the starvation case listed above.
- **Background.** `RootSlabMover` runs every `workerInterval` (0 disables it). It demotes the coldest roots while a pool has fewer than `minFreeSlabsPerPool` free slabs, up to `maxSlabsPerIteration` slabs per pool.

How a slab is demoted:

1. The coldest eligible root is picked among the roots that own slabs of the class (`AllocationClass::getRootSlabs()`), and one of its slabs.
2. The existing slab release runs (`SlabReleaseMode::kRebalance`, no receiver), so the slab goes back to the pool's free slabs and any root or class can take it. Item locking, moving and waiter handling are unchanged.
3. For this release the move callback is skipped and every item is evicted. Each regular item, with its chained items, is serialized into one slab image instead of being put to Navy one by one. Expired items follow the normal path.
4. The image is written to Navy as one object under the key `\x01\x02slab-image:<root>/<seq>`, wrapped in an `NvmItem` because Navy inspects every value as one. `RootNvmIndex` records root, image key, item count and pool.

### Promotion (Navy to DRAM)

Triggers:

- **On read miss.** `findWithRoot()` that misses DRAM while the root has images promotes all of them, then looks again.
- **Background.** The worker promotes one image at a time of the hottest root in Navy, when its pool has a free slab or holds a colder root that can give way.

`promoteRoot()` reads each image, removes it from Navy, and recreates every item in a slab of the root. If a key already exists in DRAM, the DRAM copy is newer and wins. If memory is short, the allocation path demotes a colder root first.

### Item destructors

Items go into an image marked as having a copy in Navy, so their destructor is skipped in DRAM. It runs later, either when Navy evicts the image (`NvmCache::evictCB` runs the destructor for every record) or when the image cannot be written or an item cannot be promoted. Items recreated from an image are normal DRAM items and get their destructor on their own eviction.

### Stats and config

`getRootSlabTierStats()` returns slabs and items demoted and promoted, image write failures, images Navy dropped, items lost on promotion and images in Navy. `getRootHotness(root)` returns a root's current score.

`RootSlabTierConfig`:

| Field | Default | Meaning |
|---|---|---|
| `hotnessHalfLifeSecs` | 600 | decay of a root's score |
| `workerInterval` | 0 | background worker period; 0 disables it |
| `minFreeSlabsPerPool` | 1 | free slabs the worker keeps per pool |
| `maxSlabsPerIteration` | 2 | slabs the worker demotes or promotes per pass |

### Limits of the tier

- **Region size.** An image is up to one slab (4 MB) plus per-record overhead. Navy rejects it if it does not fit a block cache region, so set the region size above that (the default 16 MB does). A rejected image drops its items and counts as an image write failure.
- **Removed keys.** `remove(key)` of a key that is only inside an image does not touch the image. A later promotion brings the key back.
- **Not persisted.** The index lives in DRAM. After a restart the images stay in Navy until Navy evicts them, and nothing promotes them.
- **Reads need the root.** Only `findWithRoot()` promotes. A plain `find()` of a demoted key misses.
- **Transient miss.** Between a slab release and the image becoming visible, and while another thread promotes a root, a read can miss.
- **Eviction cost.** Strict same-root eviction still walks the queue past other roots' items.

## Files changed (placement)

- `cachelib/allocator/memory/Slab.h`
- `cachelib/allocator/memory/AllocationClass.{h,cpp}`
- `cachelib/allocator/memory/MemoryPool.{h,cpp}`
- `cachelib/allocator/memory/MemoryAllocator.{h,cpp}`
- `cachelib/allocator/memory/serialize/objects.thrift`
- `cachelib/allocator/CacheAllocator.h`
- `cachelib/allocator/CacheVersion.h`
- `cachelib/allocator/memory/tests/AllocationClassTest.cpp`

## Tests added (`AllocationClassTest`)

- `RootsDoNotShareSlabs`: two interleaved roots get disjoint slabs; freed memory is reused only by its root; no-root allocations avoid root slabs.
- `RootsRunOutOfSlabs`: once the only slab belongs to a root, other roots and no-root allocations fail.
- `RootSlabSerializationAndRelease`: root state survives save and restore, and releasing a root's slab drops its ownership.

## Limitations

- **Starvation.** When a pool is full and every slab belongs to other roots, a new root cannot allocate. Slabs only move between allocation classes through rebalancing, never between roots, and an emptied slab is not returned to the class's free slabs.
- **Eviction cost.** Skipping other roots' items walks the eviction queue, so it gets slower as more roots share a class.
- **Unrooted paths.** Allocations that go through the plain `allocate()`, such as NVM fills, get no root.
- **Format break.** The bumped format version drops existing shared-memory caches on upgrade.

## Related

`cama/src/amt-kv/docs/ROOT_COLOCATION_PLAN_2026-10-07.md` describes the equivalent design for amt-kv, which solves the same problem outside CacheLib.

## Files changed (root slab tier)

- `cachelib/allocator/RootHotness.h` (new)
- `cachelib/allocator/RootSlabImage.h` (new): image codec, `RootNvmIndex`, `RootSlabTierConfig`, `RootSlabTierStats`
- `cachelib/allocator/RootSlabMover.h` (new)
- `cachelib/allocator/CacheAllocator.h`, `CacheAllocatorConfig.h`
- `cachelib/allocator/nvmcache/NvmCache.h`, `CacheApiWrapper.h`
- `cachelib/allocator/memory/AllocationClass.{h,cpp}`: `getRootSlabs()`
- `cachelib/allocator/tests/RootSlabTierTest.cpp` (new), `nvmcache/tests/NvmCacheTests.cpp`, `nvmcache/tests/NvmTestBase.h`

## cachebench

Options in `cache_config` (`util/CacheConfig.h`):

| Option | Default | Meaning |
|---|---|---|
| `rootPrefixLen` | 0 | leading key bytes that identify the root; 0 turns root placement off |
| `rootCount` | 0 | fold the prefix hash into this many roots; each root owns at least one slab per class it uses, so keep it small for a small cache |
| `enableRootSlabTier` | false | demote and promote slabs through Navy; needs `rootPrefixLen` and `nvmCacheSizeMB` |
| `rootHotnessHalfLifeSecs` | 600 | decay of a root's score |
| `rootSlabWorkerIntervalMs` | 0 | background worker period; 0 disables it |
| `rootMinFreeSlabsPerPool` | 1 | free slabs the worker keeps per pool |
| `rootMaxSlabsPerIteration` | 2 | slabs the worker demotes or promotes per pass |

With `rootPrefixLen` set, `Cache::allocate` calls `allocateWithRoot` and `find`, `asyncFind` and `findToWrite` call the `...WithRoot` variants. The root is the FNV-1a 64-bit hash of the first `rootPrefixLen` key bytes. The online generator writes the key index into the first key bytes, so keys spread over the roots.

When a run ends, before the cache is cleared:

- `Cache::checkRootPlacement()` sweeps the items in RAM and fails the run if an item is in a slab owned by another root. Items in an unowned slab are allowed.
- `Cache::checkRootSlabTier()` fails the run if the tier is on and no slab was demoted, no slab was promoted, or Navy rejected an image.

The tier counters print under "Root Tier" in the stats.

Configs, in `test_configs/feature_stress/root_slab/`:

- `placement.json`: RAM only, 16 roots, checks placement.
- `tier.json`: 64 roots on 128 MB with an in-memory Navy device, lookaside gets, and the background worker. Checks placement and the tier.

Both are registered in ctest as `cachebench-root-slab-placement` and `cachebench-root-slab-tier`. The config and `rootOf` unit tests are in `util/tests/CacheConfigRootTest.cpp`.

Files: `cachebench/cache/Cache.h`, `cache/CacheStats.h`, `util/CacheConfig.{h,cpp}`, `runner/CacheStressor.h`, `runner/AsyncCacheStressor.h`, `CMakeLists.txt`, `CacheAllocator::getItemRootHash`.
