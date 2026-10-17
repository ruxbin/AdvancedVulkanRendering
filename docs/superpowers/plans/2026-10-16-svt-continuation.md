# SVT continuation ledger and implementation plan

Spec: ../specs/2026-10-16-streaming-virtual-texture-design.md

User authorized continuing the existing implementation on 2026-10-16. The working tree contains the previous implementer's uncommitted M1/M2 and partial M3 files; preserve these and unrelated edits. Work in this checkout to continue that state. No existing SVT execution ledger or detailed plan was found.

Baseline: CPU test build succeeded with Windows SDK read escalation; existing 14 CTest targets passed before changes. Inspection found runtime streamingMode unused, recordUploads/commitUploads/cancelUploads declared but undefined, CPU feedback only logged, no eviction, all-slot UBO writes, and no lazy legacy blob reads.

- [x] Correct CPU scheduling budgets, retries, priorities, generation and cancellation (isolated file ownership: scheduler agent).
- [x] Bound I/O outstanding work and reject stale/duplicate results (isolated file ownership: streamer agent).
- [x] Add serial-gated physical slot retirement and tests.
- [x] Integrate streaming pool allocation, per-slot uploads/page snapshots, feedback consumption and successful-submit publication.
- [x] Finish scene compatibility, manifest checks, lazy texture ranges and UI controls.
- [x] Build CPU suite, application and shaders; run GPU checks and fix failures.
- [ ] Independent final review, verification report and usage instructions.

Contracts: runtime calls beginRead (byte reservation), then submitRead; cancelRead on rejection. AssetDesc supplies tileSize/tilePayloadBytes. Ready payloads remain in runtime until commitUploaded. evictResident removes CPU residency when runtime retires the physical page. Worker registerAsset accepts generation. Runtime owns submission serials and records only on the graphics queue; every reused slot is fence-complete before host writes. Renderer calls onSubmitted only after successful vkQueueSubmit.

Ruling: keep the existing full-residency mode as a diagnostic path, but --svt=on selects true streaming. Use coherent host-visible per-slot page-table snapshots already supported by the implementation; host writes before submit after that slot's fence provide publication, with atlas transfer barriers before sampling. This avoids an unnecessary second page-table staging allocation, at the cost of potentially slower discrete-GPU page-table reads.

Ruling: reuse inherited asset/descriptor layout (bindings 0–9, separate tail sampler and pool metadata) instead of renumbering to the illustrative design table. Validate shader/C++ layouts together.


2026-10-17 verification and decisions:
- CPU suite: 15/15 targets passed, including new TextureBlobReaderTests. SvtSystemTests now includes 19 cases; a one-slot queue reproduced incorrectly retaining mip1 instead of coarsest mip3, then passed after admission ordering fix.
- PBRT exporter: 35/35 tests passed, including byte-for-byte resident-vs-lazy texture export with a nonzero blob offset.
- Full MSVC Debug application built with DX12 enabled. Added /utf-8 for source comments and protected std::min/max from Windows macros.
- Normal GPU smoke: 240 frames, 591 admitted assets, 120.6 MiB actual atlas, 6.6 MiB actual tails, 494 uploaded pages, no validation errors or I/O failures. Original CPU blob no longer retained (686 MiB).
- 4 MiB atlas + rotating camera + synchronization validation: 720 frames, 928 uploaded pages, 751 evictions, zero I/O failures/feedback overflow. Found two depth-load hazards per frame; reproduced unchanged with --svt=off (240 errors / 120 frames). Fix preserves depth with STORE and includes attachment reads in incoming dependencies. Final sync rerun pending.
- Added optional requested/resident mip, tile grid, physical slot and fallback-depth views. Kept normal rendering the default.
- Ruling: retain legacy GPU mip textures for alpha/transparent/RT consumers, as required by the design; atlas memory is additional compatibility overhead, not a claim of lower total GPU memory.
- Ruling: validate normalized source paths, dimensions, format, mip metadata and manifest-vs-SVT content hash at admission. Recomputing original decoded texture content hashes at every launch would negate lazy I/O; unchanged-path source content changes require recooking (limitation documented).
- Ruling: keep manual mip bias for this version; automated pressure response and percentile instrumentation remain follow-up tuning rather than weakening serial safety.
- Independent final review dispatch failed because the reviewer agent hit the account usage limit. Proceed with an explicit author review; do not claim independent review passed.

- Final GPU validation: after depth STORE/dependency fixes, 720-frame on/4MiB/tour and 120-frame off both exit 0 with zero Validation Error / VUID / SYNC-HAZARD. On: 936 uploads, 760 evictions, no I/O failure/overflow. Final app and both shaders built successfully.
- Cooker rebuilt and generated/read 1024x512 diagnostic asset (42 tiles, 8 tails). git diff --check passed for tracked touched source.
- Final review: author review substituted after independent reviewer was unavailable (account quota). Per-slot publication, retirement, bounded worker cancellation, legacy compatibility and shader layouts inspected. Added scope/untested matrix explicitly to docs/svt-implementation-validation.md. Independent review remains outstanding, so its combined checkbox is intentionally not marked complete.
