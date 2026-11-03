#!/usr/bin/env python3
"""Repack Apple's Bistro ASTC archive for AAPLMeshData; never transcode pixels.

Python 3.9+, standard library only. The DXT reference supplies the exact path
hashes embedded in the shared material data (the archive does not store them).
"""
import argparse
import collections
import os
from pathlib import Path
import plistlib
import struct
import tempfile

COUNT_KEYS = ('vertexCount', 'indexCount', 'indexType', 'chunkCount', 'meshCount',
              'opaqueChunkCount', 'opaqueMeshCount', 'alphaMaskedChunkCount',
              'alphaMaskedMeshCount', 'transparentChunkCount', 'transparentMeshCount', 'materialCount')
SECTION_KEYS = ('vertexData', 'normalData', 'tangentData', 'uvData', 'indexData',
                'chunkData', 'meshData', 'materialData')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def uint(value, bits=64):
    require(type(value) is int and 0 <= value < (1 << bits), f'invalid uint{bits}: {value!r}')
    return value


def read_exact(f, size):
    require(0 <= size <= os.fstat(f.fileno()).st_size - f.tell(), 'truncated reference data')
    data = f.read(size)
    require(len(data) == size, 'short reference read')
    return data


def unpack(f, fmt):
    return struct.unpack('<' + fmt, read_exact(f, struct.calcsize('<' + fmt)))


def check_chunk(data, expected=None):
    require(len(data) >= 24, 'truncated compression header')
    mode, raw, compressed = struct.unpack_from('<I4xQQ', data)
    require(mode == 2049, f'unsupported compression mode {mode}')
    require(compressed + 24 == len(data), 'compression length mismatch')
    require(expected is None or raw == expected, f'mip/section size mismatch: {raw} != {expected}')
    return raw


def load_archive(path):
    with path.open('rb') as f:
        magic, version, length = unpack(f, '4sII')
        require(magic == b'MESH' and version == 16, 'expected MESH archive header with value 16')
        require(length == path.stat().st_size - 12, 'archive length mismatch')
        archive = plistlib.loads(read_exact(f, length))
    require(archive.get('$archiver') == 'NSKeyedArchiver', 'expected NSKeyedArchiver')
    objects = archive['$objects']

    def resolve(value):
        if isinstance(value, plistlib.UID):
            require(value.data < len(objects), 'archive object reference out of range')
            return objects[value.data]
        return value

    def array(value):
        return [resolve(v) for v in resolve(value)['NS.objects']]

    root = resolve(archive['$top']['root'])
    counts = tuple(uint(root[k]) for k in COUNT_KEYS)
    require(counts[2] == 4, 'scene requires 32-bit indices (index element size 4)')
    require(counts[5] + counts[7] + counts[9] == counts[3] and
            counts[6] + counts[8] + counts[10] == counts[4], 'scene count mismatch')
    sections = [resolve(root[k]) for k in SECTION_KEYS]
    expected = (counts[0] * 12, counts[0] * 12, counts[0] * 12, counts[0] * 8,
                counts[1] * 4, counts[3] * 96, counts[4] * 80, counts[11] * 96)
    for section, size in zip(sections, expected):
        check_chunk(section, size)
    blob = resolve(root['textureData'])
    if isinstance(blob, dict):
        blob = blob['NS.data']
    require(isinstance(blob, bytes), 'expected texture data bytes')
    textures = []
    paths = set()
    for entry in array(root['textures']):
        tex = dict(entry)
        tex['path'] = resolve(tex['path'])
        path_bytes = tex['path'].encode('utf-8')
        require(0 < len(path_bytes) <= 4096 and b'\0' not in path_bytes, 'invalid texture path')
        require(tex['path'] not in paths, f'duplicate archive path: {tex["path"]}')
        paths.add(tex['path'])
        for k in ('width', 'height', 'mipmapLevelCount', 'pixelDataOffset', 'pixelDataLength'):
            uint(tex[k])
        require(tex['pixelFormat'] in (186, 204), f'unsupported ASTC format {tex["pixelFormat"]}')
        w, h, mips = tex['width'], tex['height'], tex['mipmapLevelCount']
        require(0 < w <= 16384 and 0 < h <= 16384 and
                w & (w - 1) == 0 and h & (h - 1) == 0, 'runtime requires power-of-two texture dimensions')
        require(0 < mips <= max(w, h).bit_length(), 'invalid mip count')
        offsets, lengths = array(tex['mipOffsets']), array(tex['mipLengths'])
        require(len(offsets) == len(lengths) == mips, 'mip count/array length mismatch')
        base, size = tex['pixelDataOffset'], tex['pixelDataLength']
        require(base + size <= len(blob), 'texture range outside blob')
        end = 0
        for m, (offset, length) in enumerate(zip(offsets, lengths)):
            uint(offset)
            uint(length)
            require(end <= offset and offset + length <= size, 'mip range outside texture or overlapping')
            end = offset + length
            expected_bytes = ((max(w >> m, 1) + 3) // 4) * ((max(h >> m, 1) + 3) // 4) * 16
            check_chunk(memoryview(blob)[base + offset:base + end], expected_bytes)
        tex['mipOffsets'], tex['mipLengths'] = offsets, lengths
        textures.append(tex)
    require(0 < len(textures) <= 65536, 'invalid texture count')
    return counts, sections, textures, blob


def reference_hashes(path, counts, sections):
    hashes = {}
    used_hashes = set()
    with path.open('rb') as f:
        require(unpack(f, '12Q') == counts, 'reference scene counts differ from archive')
        for key, section in zip(SECTION_KEYS, sections):
            length, = unpack(f, 'Q')
            require(length == len(section), f'reference {key} length differs')
            data = read_exact(f, length)
            # Bytes 4..7 are ABI padding, not serialized scene content.
            require(data[:4] == section[:4] and memoryview(data)[8:] == memoryview(section)[8:],
                    f'reference {key} differs from archive')
        count, = unpack(f, 'Q')
        require(0 < count <= 65536, 'invalid reference texture count')
        for _ in range(count):
            length, = unpack(f, 'I')
            require(0 < length <= 4096, 'invalid reference path length')
            texture_path = read_exact(f, length).decode('utf-8')
            hash_value, w, h, mips, fmt, offset, size = unpack(f, 'IQQQIQQ')
            require(0 < mips <= 32, 'invalid reference mip count')
            read_exact(f, 16 * mips)
            require(texture_path not in hashes, f'duplicate reference path: {texture_path}')
            require(hash_value not in used_hashes, f'duplicate reference hash: {hash_value}')
            hashes[texture_path] = hash_value
            used_hashes.add(hash_value)
        blob_size, = unpack(f, 'Q')
        require(f.tell() + blob_size == path.stat().st_size, 'reference blob length mismatch')
    return hashes


def convert(source, reference, output):
    source, reference, output = map(Path, (source, reference, output))
    for original in (source, reference):
        require(output.resolve() != original.resolve() and
                not (output.exists() and os.path.samefile(output, original)),
                'output must not overwrite either input file')
    counts, sections, textures, blob = load_archive(source)
    hashes = reference_hashes(reference, counts, sections)
    require({t['path'] for t in textures} == set(hashes), 'archive/reference texture path sets differ')
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=output.parent, prefix=output.name + '.', suffix='.tmp', delete=False) as f:
            temporary = Path(f.name)
            f.write(struct.pack('<12Q', *counts))
            for section in sections:
                f.write(struct.pack('<Q', len(section)))
                f.write(section)
            f.write(struct.pack('<Q', len(textures)))
            for tex in textures:
                path = tex['path'].encode('utf-8')
                f.write(struct.pack('<I', len(path)))
                f.write(path)
                f.write(struct.pack('<IQQQIQQ', hashes[tex['path']], tex['width'], tex['height'],
                                    tex['mipmapLevelCount'], tex['pixelFormat'],
                                    tex['pixelDataOffset'], tex['pixelDataLength']))
                for key in ('mipOffsets', 'mipLengths'):
                    f.write(struct.pack('<' + 'Q' * len(tex[key]), *tex[key]))
            f.write(struct.pack('<Q', len(blob)))
            f.write(blob)
            f.flush()
            os.fsync(f.fileno())
        os.replace(temporary, output)
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()
    return collections.Counter(t['pixelFormat'] for t in textures)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, help='original bistro.astc.bin (MESH archive)')
    parser.add_argument('--reference', required=True, type=Path, help='matching sequential bistro.dxt.bin')
    parser.add_argument('--output', required=True, type=Path, help='separate runtime .bin output')
    args = parser.parse_args()
    try:
        formats = convert(args.source, args.reference, args.output)
    except (OSError, ValueError, KeyError, TypeError, IndexError, OverflowError, struct.error) as exc:
        parser.exit(1, f'error: {exc}\n')
    print(f'Wrote {args.output}: {args.output.stat().st_size} bytes; '
          f'{sum(formats.values())} textures; formats {dict(formats)}. Inputs unchanged.')


if __name__ == '__main__':
    main()
