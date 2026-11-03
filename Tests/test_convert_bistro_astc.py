import pathlib
import plistlib
import struct
import subprocess
import sys
import tempfile
import unittest

TOOL = pathlib.Path(__file__).resolve().parents[1] / 'Tools/convert_bistro_astc.py'
COUNTS = (1, 3, 4, 1, 1, 1, 1, 0, 0, 0, 0, 1)
COUNT_KEYS = ('vertexCount', 'indexCount', 'indexType', 'chunkCount', 'meshCount',
              'opaqueChunkCount', 'opaqueMeshCount', 'alphaMaskedChunkCount',
              'alphaMaskedMeshCount', 'transparentChunkCount', 'transparentMeshCount', 'materialCount')
SECTION_KEYS = ('vertexData', 'normalData', 'tangentData', 'uvData', 'indexData',
                'chunkData', 'meshData', 'materialData')


def chunk(raw):
    # A real LZFSE uncompressed block, not GPU pixel decompression.
    payload = b'bvx-' + struct.pack('<I', len(raw)) + raw + b'bvx$'
    return struct.pack('<I4xQQ', 2049, len(raw), len(payload)) + payload


class ConverterTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = pathlib.Path(self.tmp.name)
        self.source = self.root / 'source.bin'
        self.reference = self.root / 'reference.bin'
        self.output = self.root / 'runtime.bin'
        self.objects = ['$null']

        def obj(value):
            self.objects.append(value)
            return plistlib.UID(len(self.objects) - 1)

        self.mesh = dict(zip(COUNT_KEYS, COUNTS))
        self.sections = [chunk(bytes([i + 1]) * n) for i, n in
                         enumerate((12, 12, 12, 8, 12, 96, 80, 96))]
        for k, v in zip(SECTION_KEYS, self.sections):
            self.mesh[k] = obj(v)
        self.textures = []
        self.blob = b''
        for i, fmt in enumerate((186, 204)):
            mip = chunk(bytes([40 + i]) * 16)
            tex = dict(path=obj(f'Textures/{i}.dds'), width=4, height=4,
                       mipmapLevelCount=3, pixelFormat=fmt,
                       pixelDataOffset=len(self.blob), pixelDataLength=len(mip) * 3,
                       mipOffsets=obj({'NS.objects': [obj(n * len(mip)) for n in range(3)]}),
                       mipLengths=obj({'NS.objects': [obj(len(mip)) for _ in range(3)]}))
            self.textures.append(tex)
            self.blob += mip * 3
        self.mesh['textures'] = obj({'NS.objects': [obj(t) for t in self.textures]})
        self.mesh['textureData'] = obj({'NS.data': self.blob})
        self.archive = {'$archiver': 'NSKeyedArchiver', '$version': 100000,
                        '$objects': self.objects, '$top': {'root': obj(self.mesh)}}
        self.write_source()
        self.write_reference()

    def write_source(self):
        data = plistlib.dumps(self.archive, fmt=plistlib.FMT_BINARY, sort_keys=False)
        self.source.write_bytes(struct.pack('<4sII', b'MESH', 16, len(data)) + data)

    def write_reference(self, duplicate=False):
        with self.reference.open('wb') as f:
            f.write(struct.pack('<12Q', *COUNTS))
            for section in self.sections:
                f.write(struct.pack('<Q', len(section)) + section)
            f.write(struct.pack('<Q', 2))
            for i in range(2):
                path = f'Textures/{0 if duplicate else i}.dds'.encode()
                f.write(struct.pack('<I', len(path)) + path)
                f.write(struct.pack('<IQQQIQQ', 100 + i, 4, 4, 3,
                                    135 if i == 0 else 142, i * 156, 156))
                f.write(struct.pack('<6Q', 0, 52, 104, 52, 52, 52))
            f.write(struct.pack('<Q', len(self.blob)) + self.blob)

    def run_tool(self, output=None, success=True):
        result = subprocess.run([sys.executable, str(TOOL), str(self.source),
                                 '--reference', str(self.reference),
                                 '--output', str(output or self.output)], capture_output=True, text=True)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('error:', result.stderr.lower())
        return result

    def test_repack_preserves_payload_hashes_and_source_files(self):
        originals = (self.source.read_bytes(), self.reference.read_bytes())
        self.run_tool()
        with self.output.open('rb') as f:
            self.assertEqual(struct.unpack('<12Q', f.read(96)), COUNTS)
            for section in self.sections:
                n, = struct.unpack('<Q', f.read(8))
                self.assertEqual(f.read(n), section)
            self.assertEqual(struct.unpack('<Q', f.read(8))[0], 2)
            for i, fmt in enumerate((186, 204)):
                n, = struct.unpack('<I', f.read(4))
                self.assertEqual(f.read(n), f'Textures/{i}.dds'.encode())
                self.assertEqual(struct.unpack('<IQQQIQQ', f.read(48)),
                                 (100 + i, 4, 4, 3, fmt, i * 156, 156))
                self.assertEqual(struct.unpack('<6Q', f.read(48)), (0, 52, 104, 52, 52, 52))
            self.assertEqual(struct.unpack('<Q', f.read(8))[0], len(self.blob))
            self.assertEqual(f.read(), self.blob)
        self.assertEqual((self.source.read_bytes(), self.reference.read_bytes()), originals)

    def test_cannot_overwrite_either_input(self):
        for path in (self.source, self.reference):
            original = path.read_bytes()
            self.assertIn('input', self.run_tool(output=path, success=False).stderr)
            self.assertEqual(path.read_bytes(), original)

    def test_hardlink_to_input_rejected(self):
        self.output.hardlink_to(self.source)
        original = self.source.read_bytes()
        self.run_tool(success=False)
        self.assertEqual(self.source.read_bytes(), original)

    def test_missing_path_keeps_existing_output(self):
        self.objects[self.textures[0]['path'].data] = 'missing.dds'
        self.write_source()
        self.output.write_bytes(b'previous good asset')
        self.assertIn('path', self.run_tool(success=False).stderr)
        self.assertEqual(self.output.read_bytes(), b'previous good asset')

    def test_duplicate_reference_path_rejected(self):
        self.write_reference(duplicate=True)
        self.assertIn('duplicate', self.run_tool(success=False).stderr)

    def test_mismatched_materials_rejected(self):
        self.objects[self.mesh['materialData'].data] = chunk(b'X' * 96)
        self.write_source()
        self.assertIn('materialData', self.run_tool(success=False).stderr)

    def test_invalid_mip_range_rejected(self):
        self.textures[0]['pixelDataOffset'] = len(self.blob)
        self.write_source()
        self.assertIn('range', self.run_tool(success=False).stderr)

    def test_wrong_mip_size_rejected(self):
        self.textures[0]['width'] = 16
        self.write_source()
        self.assertIn('mip', self.run_tool(success=False).stderr)

    def test_unsupported_format_rejected(self):
        self.textures[0]['pixelFormat'] = 194
        self.write_source()
        self.assertIn('format', self.run_tool(success=False).stderr)

    def test_truncated_archive_rejected(self):
        self.source.write_bytes(self.source.read_bytes()[:-1])
        self.assertIn('length', self.run_tool(success=False).stderr)


if __name__ == '__main__':
    unittest.main()
