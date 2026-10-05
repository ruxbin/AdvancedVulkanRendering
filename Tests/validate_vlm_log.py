"""Fail closed on missing/invalid bake diagnostics; optional asset superposition check.

Superposition is a linearity regression, not proof of unique light ownership or
absolute energy. Use a dark baseline to remove material emission/local sources.
"""
import argparse
import math
import pathlib
import re
import struct
import zlib

NUMBER = r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?|nan|inf"


def metric(text, label, key, limit):
    line = next((line for line in text.splitlines() if label in line), None)
    if not line:
        raise ValueError(f"missing {label}")
    match = re.search(rf"\b{key}=({NUMBER})\b", line, re.IGNORECASE)
    if not match:
        raise ValueError(f"missing {key} in {label}")
    value = float(match[1])
    if not math.isfinite(value) or value < 0 or value > limit:
        raise ValueError(f"{label} {key}={value} exceeds {limit}")
    return value


def validate_log(text, mode=None):
    if re.search(r"VUID-|Validation Error|vlm.*(?:FAILED|\[error\])", text, re.IGNORECASE):
        raise ValueError("bake/validation error in log")
    written=re.search(r"vlm bake: wrote .+\((\d+) probes, \d+ bytes\)", text)
    if not written:
        raise ValueError("missing successful asset write")
    if "vlm: runtime activated" not in text:
        raise ValueError("missing runtime activation")
    result = {"quantization": metric(text, "vlm validate quantization:", "maxRelErr", .01)}
    if mode == "const":
        result["constSky"] = metric(text, "vlm validate const-sky:", "maxRelErr", .01)
    if mode == "direction":
        result["directionC0"] = metric(text, "vlm validate direction:", "c0RelErr", .01)
        result["directionWeighted"] = metric(text, "vlm validate direction:", "weightedRelErr", .05)
    probes = re.findall(rf"vlm probe (\d+) E\(\+Y\)=({NUMBER}),({NUMBER}),({NUMBER})", text, re.IGNORECASE)
    ids={int(p[0]) for p in probes}
    if ids != set(range(min(8,int(written[1])))) or len(ids) != len(probes):
        raise ValueError("missing/duplicate probe diagnostics")
    if not all(math.isfinite(float(c)) for p in probes for c in p[1:]):
        raise ValueError("nonfinite probe irradiance")
    return result


def read_asset(path):
    raw = pathlib.Path(path).read_bytes()
    if len(raw) < 132 or struct.unpack_from("<5I", raw) != (0x314D4C56, 1, 0x01020304, 1, 1):
        raise ValueError(f"{path}: invalid header")
    length, crc, count = struct.unpack_from("<QII", raw, 24)
    if length != len(raw) or len(raw) != 132 + 57*count or not 0 < count <= 1_000_000:
        raise ValueError(f"{path}: invalid length/count")
    if zlib.crc32(raw[64:]) != crc:
        raise ValueError(f"{path}: CRC mismatch")
    if struct.unpack_from("<I", raw, 72)[0] != 3:
        raise ValueError(f"{path}: unsupported integrator")
    validity = raw[132:132+count]
    if any(v not in (0, 1) for v in validity):
        raise ValueError(f"{path}: invalid validity")
    coefficients = struct.unpack_from(f"<{count*28}e", raw, 132+count)
    if not all(math.isfinite(v) for v in coefficients) or any(coefficients[i*28+27] != 0 for i in range(count)):
        raise ValueError(f"{path}: invalid coefficients/padding")
    # Layout, band, units and validity must agree; lighting identity is expected to differ.
    layout = (raw[80:124], validity)
    return layout, coefficients


def validate_linearity(paths):
    assets = [read_asset(p) for p in paths]
    if any(a[0] != assets[0][0] for a in assets):
        raise ValueError("linearity fixtures have different layouts/validity")
    dark, env, sun, both = [a[1] for a in assets]
    reference = [e+s-d for d,e,s in zip(dark,env,sun)]
    # A global energy floor makes near-zero, cancellation-dominated coefficients stable.
    floor = max(1e-4, max(abs(v) for v in reference)*.01)
    error = max(abs(b-r)/max(floor,abs(r)) for b,r in zip(both,reference))
    if error > .01:
        raise ValueError(f"FP16 coefficient superposition error {error} > .01")
    return error


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", nargs="?")
    parser.add_argument("--mode", choices=("const", "direction"))
    parser.add_argument("--asset")
    parser.add_argument("--linearity", nargs=4, metavar=("DARK", "ENV", "SUN", "BOTH"))
    args = parser.parse_args()
    try:
        if not args.log and not args.linearity:
            parser.error("provide a log or --linearity")
        if args.log:
            print(validate_log(pathlib.Path(args.log).read_text(encoding="utf-8-sig"), args.mode))
        if args.asset:
            read_asset(args.asset)
        if args.linearity:
            print({"coefficientSuperposition": validate_linearity(args.linearity)})
    except (ValueError, OSError, struct.error) as error:
        parser.exit(1, f"FAIL: {error}\n")


if __name__ == "__main__":
    main()
