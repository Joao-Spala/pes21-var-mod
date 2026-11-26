# Engine heap segment release (post-match crash)

v4.4.1 research. Addresses of `PES2021.exe` Steam `1.7.2.0`. **[P]** proven by disassembly or dump, **[I]** inferred.

## 1. Symptom

- After the final whistle, when advancing, the game froze and, when closed, produced the dump. Both dumps (`PES2021.exe.4568.dmp` from v4.2.1 and `PES2021.exe.19524.dmp`, full, from v4.2.4) show the same fault: a write to unmapped memory at `PES2021.exe+0x3FE31D`, with the same heap state `rsi = 0x56F0010` **[P]**.
- The freeze is the crash itself: Windows writes a full dump of about 5 GB before terminating the process **[I]**.

## 2. Where

- The faulting thread is in the scene state machine (`0x141FE0A80` onward), which calls `0x1403FB8B0`. That function walks the 50 heaps at `*(0x1436FD708)` and calls the `+0x50` method of each one, which compacts the heap **[P]**.
- The heap is a dlmalloc: segments with a footer `{base, size, next, flags}` in the last `0x40` bytes, bins at `mstate + 0x258`, total at `+0x358` **[P]**.
- `release_unused_segments` (loop at `0x1403FE1C0`) calls `win32munmap` (`0x1403FC0D0`) for each free segment. If it returns `-1`, the code assumes nothing was released and reinserts the chunk into the bins by writing to `chunk + 0x38` (`0x1403FE31D`) **[P]**.

## 3. Why win32munmap fails after releasing

- `win32munmap` walks the range with `VirtualQuery` and requires, in each region, `BaseAddress == AllocationBase == address`, state `MEM_COMMIT` and a size within what remains. For each region it calls `VirtualFree(MEM_RELEASE)` **[P]**.
- `VirtualFree(MEM_RELEASE)` always releases the whole allocation. When an allocation has pages with different protections, `VirtualQuery` returns only the first region. The function releases the whole allocation, queries the next region, finds free memory and returns `-1` **[P]**.
- In the full dump, the region immediately before the segment that crashed (`0x118500000`) has stray `PAGE_EXECUTE_READWRITE` (`0x40`) pages inside a `PAGE_READWRITE` block. The `mstate` page itself at `0x56F0000` is `0x40` **[P]**.
- The segment that crashed (`0x118550000`, size `0x50000`) was mapped before the call, because the loop reads `0x18(%rbx)` and `0x30(%rbx)` before calling the release, and was completely free in the dump. This confirms a half-done release within the call itself **[P]**.

## 4. Who changes the protection

- `var_core.dll` only calls `VirtualProtect` on code (`.text`), vtables and the IAT, never on heap data **[P]**.
- Dozens of Lua patch modules use Sider `memory.write` on game data addresses **[P]**. Leaving pages with execute protection is the observed effect **[I]**.
- The defect is independent of the VAR. More activity in the match only raises the chance that an affected segment is released during the trim **[I]**.

## 5. Fix

- `heap_guard.cpp` replaces `win32munmap` (unique AOB, hint `0x3FC0D0`) with an atomic version:
  - walks the range by allocation (`AllocationBase`), not by protection region
  - validates the whole range before releasing anything: each allocation starts where the previous one ends, is entirely in `MEM_COMMIT` and fits in the range
  - only then releases allocation by allocation and returns `0`. If validation fails, it returns `-1` without touching anything, and dlmalloc keeps the chunk mapped.
- The six callers (`0x1403FC717`, `0x1403FC77A`, `0x1403FD410`, `0x1403FE2C3`, `0x1403FE6D3`, `0x1403FE7D8`) handle `0` and `-1` under the same contract **[P]**.
- The hook is installed in `var_init`, separate from the match hooks, and stays active for the whole process.
- Tests: pattern on the real binary, planning with the dump layout and a real release with a `PAGE_EXECUTE_READWRITE` page in the middle of the allocation.
