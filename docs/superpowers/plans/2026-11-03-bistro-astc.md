# Bistro ASTC implementation plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Convert the existing Apple ASTC archive without changing either source asset, and load the converted scene on Android with explicit Vulkan format checks.

**Architecture:** A Python standard-library tool resolves the keyed archive, validates compressed section/mip boundaries, and copies its bytes into the existing sequential layout. Texture hashes come from the DXT reference by exact path. Android builds generate a separate runtime asset; shared Vulkan format helpers reject unsupported devices before uploading.

**Tech Stack:** Python 3, C++17/20, Vulkan, Android Gradle/NDK.

**Spec:** User-approved proposal in this chat (2026-11-03): offline repack, ASTC mapping, device support checks, preserve originals.

## Global constraints
- Keep bistro.astc.bin and bistro.dxt.bin unchanged.
- Output bistro.astc.runtime.bin; preserve ASTC/LZFSE payload bytes and material hashes.
- Support the actual formats 186 (ASTC 4x4 sRGB) and 204 (ASTC 4x4 UNORM); retain BC behavior.
- Reuse this checkout and preserve pre-existing edits, including Tests/CMakeLists.txt.

## Review focus
- Output aliasing either source must fail before writing (including symlinks/hardlinks).
- Missing/duplicate paths, mismatched scene sections, truncated chunks and invalid mip ranges must fail clearly.
- Unsupported ASTC sampling/linear filtering/transfer must fail before image creation.
- Stale raw ASTC assets must not be packaged alongside generated resources.
- Real asset verification must check the emitted layout and every compressed mip, not just the converter's exit status.

## Tasks
- [x] 1. Add converter regression tests using a small real plist fixture and independently parse output; observe failure, implement Tools/convert_bistro_astc.py, run tests. Validate full source assets and hashes.
- [x] 2. Add C++ tests for ASTC/BC mapping and unsupported capability handling; observe failure, implement shared helper and connect GpuScene. Make Android binary reads exact and reject raw MESH input before allocations.
- [x] 3. Wire incremental Gradle conversion/packaging, document CLI and Python override, compile native Android target and run CPU/Python tests. Record any environmental or pre-existing Android blockers precisely.
- [x] 4. Fresh code review, address material findings, final verification and report.

## Execution notes
- User already requested implementation of the reviewed proposal; no additional design approval needed.
- Changes remain uncommitted for review in the existing dirty checkout.
- Ruling: repair the Android CMake source list, which omitted current renderer modules and failed to link. Expose initialized RT property accessors for shared-code compilation; Android RT enablement remains false.
- Ruling: exclude historical generated `debug1.bin` as well as raw Bistro files from staging, preserving originals on disk. Inspection found the old file would otherwise add 800 MiB to the APK.
- Build environment: MSBuild required SDK access outside sandbox; Java required a shorter `java.io.tmpdir`/`jdk.net.unixdomain.tmpdir`; SDL's local ccache hung, so this validation cache uses `SDL_CCACHE=OFF`. Gradle dependencies for the existing AGP 8.2.0 version were fetched; no project tool versions were changed.

## Verification ledger (2026-11-03)
- Converter regression: 10 tests passed after the initial missing-tool failures.
- C++ mapping/reader regressions: passed after initial missing-interface compile failures.
- `ctest --test-dir build-astc-tests -C Debug --output-on-failure`: 22/22 passed.
- `python -m unittest discover -s Tests -p 'test_*.py'`: 47/47 passed (includes converter tests).
- Full asset acceptance: existing sequential C++ parser plus LZFSE decoded all 8 scene sections and 4815 ASTC mips, 455918352 GPU block bytes.
- Gradle `:app:assembleDebug`: successful; ARM64 `libmain.so` linked. APK contains only `assets/bistro.astc.runtime.bin` as a binary scene asset, ZIP_STORED, byte-identical to the standalone output.
- Original ASTC SHA-256: `e54467280e850aa2ec6b0cb1c28764eae593d39e036954a292092c848a774906` (unchanged).
- Original DXT SHA-256: `ea2109e56db50988beee9bc409def05f161f65659fa952acf4936424ef465f36` (unchanged).
- Runtime SHA-256: `12ee0acae31ad8685522cdd12e0047a1647b0793f0f8b26ec6192d715a9b7dc0` (standalone, generated, merged, and APK copies match).
- Independent read-only review: no Critical/Important findings.
- Final: minor (deferred): malformed plist field types can produce an AttributeError traceback instead of the concise CLI diagnostic; input/output preservation is unaffected.
- Final: Ruling: reviewer did not judge device rendering or final NDK link. Parent verified NDK and APK build; no adb devices connected, so GPU/visual validation remains explicitly unverified.
