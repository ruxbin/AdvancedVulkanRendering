# Bistro ASTC assets on Android

The supplied `bistro.astc.bin` is an Apple `MESH` / NSKeyedArchiver archive. It
cannot be passed to the engine's sequential binary reader. Keep that original
and `bistro.dxt.bin`; neither is modified by the converter.

Run from the repository root with Python 3.9 or newer (no pip packages needed):

```sh
python Tools/convert_bistro_astc.py bistro.astc.bin --reference bistro.dxt.bin --output bistro.astc.runtime.bin
python -m unittest discover -s Tests -p test_convert_bistro_astc.py
```

The reference must describe the same scene. The tool verifies counts and all
eight geometry/material sections (ignoring compression-header ABI padding),
matches texture paths, and copies the reference path hashes. It validates every
ASTC mip's range and declared size, then copies the compressed payloads unchanged.
It does not decode/re-encode ASTC or recompress LZFSE. Output is atomically replaced
only after successful conversion; overwriting either input (including an alias)
is rejected.

Android Gradle automatically runs `convertBistroAstc` when inputs change. Its
output is `android/app/build/generated/bistro/bistro.astc.runtime.bin`; `copyAssets`
stages it with shaders and the scene in `build/generated/bistroAssets`. Only that
staging directory is packaged. Old `bistro*.bin` copies in `src/main/assets` are
excluded, as is the legacy `debug1.bin` scene copy; those files are not deleted.
Custom non-generated assets are retained.

```sh
cd android
./gradlew :app:assembleDebug
# Override the Python executable when necessary (Windows: use gradlew.bat):
./gradlew :app:assembleDebug -PbistroPython=/usr/bin/python3
```

The engine opens `bistro.astc.runtime.bin` on Android and still opens
`bistro.dxt.bin` on desktop. Metal formats 186 and 204 map to Vulkan ASTC 4x4
sRGB and UNORM respectively. Before allocating textures it checks ASTC LDR
support, optimal-tiling sampling/linear filtering, transfer source/destination
support, and image format limits. Unsupported devices receive a descriptive
error; there is no automatic transcode/fallback to another compression format.

This asset contains 591 textures, including 326 at 1024x1024 (the DXT reference
has 2048x2048 for those textures). The runtime still retains the ~195 MiB texture
blob in host memory; conversion does not implement Android file-range streaming.

CPU regression tests:

```sh
cmake -S Tests -B build-astc-tests
cmake --build build-astc-tests --config Debug
ctest --test-dir build-astc-tests -C Debug --output-on-failure
```

When the standalone runtime file exists at configure time, CTest also runs
`BistroRuntimeAssetTests`: the existing C++ directory parser and LZFSE library
read/decode all eight scene sections and all 4815 mip payloads.

Validation on 2026-11-03: 22 CTest cases and 47 Python tests passed; ARM64 native
link and `:app:assembleDebug` passed. APK contents were checked to contain the
323655156-byte runtime asset uncompressed, matching the standalone output's
SHA-256. No Android device was connected, so visual output and device memory
behavior have not been verified.
