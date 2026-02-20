# QuGate Failure Log

Timestamped log of build failures, node crashes, and test failures.

---

## 2026-02-18 15:22 GMT — Node startup crash (e01)

**Context:** V2 QuGate.h compiled successfully. Started node without watchdog or swap.  
**Cause:** Watchdog not restarted after rebuild. Swap not in fstab (null bytes corrupted entry).  
**Resolution:** Fixed fstab (removed null bytes), created persistent systemd watchdog service.

---

## 2026-02-18 15:55 GMT — V2 test script gate ID mismatch

**Context:** Running V1 test scripts against V2 contract with free-list slot reuse.  
**Symptom:** ROUND_ROBIN test reads wrong gate (shows mode=SPLIT for newly created ROUND_ROBIN gate).  
**Cause:** Test scripts use `gate_id = query_gate_count().total` to find the newest gate. With V2 free-list, closed gates push their slot to the free-list. New gates reuse old slots, so `_gateCount` (high-water mark) doesn't equal the latest gate ID.  
**Impact:** Test scripts report wrong results, but **contract logic is correct** — balance changes confirm proper routing.  
**Resolution needed:** Test scripts need updating — each test should run independently on a fresh node, or track gate IDs explicitly rather than inferring from gate count.
