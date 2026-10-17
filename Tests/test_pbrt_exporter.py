"""Offline regressions: compile and execute the real exporter with CPU scene data.

Run: python Tests/test_pbrt_exporter.py
CXX may select a C++20 compiler (defaults to g++); no Vulkan device is created.
"""
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def read_pfm(path):
    with path.open('rb') as f:
        magic = f.readline().strip()
        width, height = map(int, f.readline().split())
        scale = float(f.readline())
        data = f.read()
    channels = 3 if magic == b'PF' else 1
    assert magic in (b'PF', b'Pf')
    values = struct.unpack(('<' if scale < 0 else '>') + 'f' * (width*height*channels), data)
    stride = width*channels
    # PFM stores scanlines bottom-up; tests reason in source top-down order.
    rows = [values[y*stride:(y+1)*stride] for y in range(height)]
    return width, height, [v for row in reversed(rows) for v in row]


class ExporterTests(unittest.TestCase):
    def test_streetlight_clearance_removes_near_open_shade_faces(self):
        for mode in ('proxy_street_metal','proxy_street_emissive'):
            with self.subTest(mode=mode):
                d,scene=self.export(mode)
                ply=(d/'mesh_0.ply').read_bytes()
                self.assertIn(b'element face 1\n',ply)
                self.assertEqual(struct.unpack_from('<fff',ply.split(b'end_header\n')[1]),(10,0,0))
                self.assertEqual(scene.count('LightSource "point"'),1)

    def test_streetlight_clearance_preserves_far_and_unrelated_faces(self):
        for mode in ('proxy_street_far','proxy_street_unrelated'):
            with self.subTest(mode=mode):
                d,_=self.export(mode)
                self.assertIn(b'element face 2\n',(d/'mesh_0.ply').read_bytes())

    def test_point_proxy_removes_only_enclosing_emissive_component(self):
        for mode in ('proxy_partial','proxy_seams','proxy_16','proxy_mixed'):
            with self.subTest(mode=mode):
                d,scene=self.export(mode)
                ply=(d/'mesh_0.ply').read_bytes()
                self.assertIn(b'element face 4\n',ply)
                # Surviving component is the tetrahedron at x=10, not the
                # enclosing bulb at x=0, even when both share one material.
                body=ply.split(b'end_header\n',1)[1]
                self.assertEqual(struct.unpack_from('<fff',body),(10,0,0))
                self.assertEqual(scene.count('LightSource "point"'),1)
                self.assertIn('AreaLightSource "diffuse"',scene)
                if mode=='proxy_mixed':
                    self.assertIn(b'element face 4\n',(d/'mesh_1.ply').read_bytes())
                    self.assertIn('Texture "color_1"',scene)
                    self.assertTrue((d/'texture_77.bmp').exists())

    def test_point_proxy_preserves_unmatched_and_opaque_geometry(self):
        for mode,faces in (('proxy_outside',8),('proxy_none',8),('proxy_opaque',8),('proxy_open',7)):
            with self.subTest(mode=mode):
                d,_=self.export(mode)
                self.assertIn(f'element face {faces}\n'.encode(),(d/'mesh_0.ply').read_bytes())

    def test_point_proxy_all_removed_does_not_emit_materials_or_area_lights(self):
        d,scene=self.export('proxy_all')
        self.assertFalse((d/'mesh_0.ply').exists())
        self.assertNotIn('AreaLightSource',scene)
        self.assertNotIn('MakeNamedMaterial',scene)
        self.assertEqual(list(d.glob('*.bmp')),[])
        self.assertEqual(list(d.glob('*.pfm')),[])
        self.assertEqual(scene.count('LightSource "point"'),2)

    def test_point_proxy_accepts_bulb_with_small_open_neck(self):
        d,scene=self.export('proxy_small_opening')
        self.assertFalse((d/'mesh_0.ply').exists())
        self.assertNotIn('AreaLightSource',scene)
        self.assertEqual(scene.count('LightSource "point"'),1)

    def test_point_proxy_preserves_sheet_touching_bulb_at_one_vertex(self):
        d,_=self.export('proxy_vertex_attachment')
        ply=(d/'mesh_0.ply').read_bytes()
        self.assertIn(b'element face 5\n',ply)
        self.assertEqual(ply[-13:],struct.pack('<Biii',3,0,8,9))

    def test_point_proxy_combines_shell_split_across_emissive_materials(self):
        d,scene=self.export('proxy_split_materials')
        self.assertEqual(list(d.glob('*.ply')),[])
        self.assertEqual(list(d.glob('*.bmp')),[])
        self.assertNotIn('AreaLightSource',scene)
        self.assertEqual(scene.count('LightSource "point"'),1)

    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='pbrt-exporter-build-')
        cls.exe = Path(cls.build.name)/'fixture.exe'
        objects=[]
        for source in ('lzfse_decode.c','lzfse_decode_base.c','lzfse_fse.c','lzvn_decode_base.c'):
            obj=Path(cls.build.name)/(source+'.o')
            subprocess.run([os.environ.get('CC','gcc'),'-std=c99','-O0','-c',
                            str(ROOT/'ThirdParty/lzfse/src'/source),'-o',str(obj)],check=True)
            objects.append(str(obj))
        subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++20', '-O0',
                        # Static link: the suite must not depend on a MinGW
                        # runtime being on PATH (a foreign libstdc++ on PATH
                        # otherwise makes every test error out).
                        '-static',
                        '-I'+str(ROOT/'Tests/pbrt_fixture'),
                        '-I'+str(ROOT/'Src/Include'),
                        '-I'+str(ROOT/'ThirdParty/spdlog/include'),
                        str(ROOT/'Tests/PbrtExporterFixture.cpp'), str(ROOT/'Src/TextureBlobReader.cpp'), *objects, '-o', str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def export(self, mode, expected='success'):
        tmp = tempfile.TemporaryDirectory(prefix='pbrt-exporter-case-')
        self.addCleanup(tmp.cleanup)
        directory = Path(tmp.name)
        subprocess.run([str(self.exe), mode, str(directory)], check=True, capture_output=True, timeout=60)
        if mode not in ('bc1','bc3'):
            self.assertEqual((directory/'result.txt').read_text(),expected)
        scene = directory/'scene.pbrt'
        return directory, scene.read_text() if scene.is_file() else ''

    def texture(self, directory, scene, name):
        match = re.search(r'Texture "'+name+r'"[^\n]*\n\s*"string filename" "([^"]+)"', scene)
        self.assertIsNotNone(match, 'expected a declared image texture')
        path = directory/match[1]
        self.assertEqual(path.suffix, '.pfm', 'numeric maps must be linear float images')
        return read_pfm(path)

    def test_lazy_blob_matches_resident_texture_export(self):
        resident, resident_scene = self.export('mask')
        lazy, lazy_scene = self.export('lazy_mask')
        self.assertEqual(resident_scene, lazy_scene)
        for image in list(resident.glob('*.pfm')) + list(resident.glob('*.bmp')):
            self.assertEqual(image.read_bytes(), (lazy/image.name).read_bytes())

    def test_bc3_uses_four_colors_even_when_endpoints_ascending(self):
        d, _ = self.export('bc3')
        self.assertEqual((d/'pixels.bin').read_bytes()[:4], bytes([170,170,170,255]))

    def test_bc1_keeps_transparent_mode(self):
        d, _ = self.export('bc1')
        self.assertEqual((d/'pixels.bin').read_bytes()[:4], bytes([0,0,0,0]))

    def test_point_lights_preserve_world_positions_independent_of_camera(self):
        _,scene = self.export('point_positions')
        world=scene.split('WorldBegin',1)[1]
        positions=re.findall(r'LightSource "point"\s+"point3 from"\s*\[([^]]+)\]',world)
        self.assertEqual([tuple(map(float,p.split())) for p in positions],
                         [(-19.25,-5.5,18.75),(7.125,3.25,-11.5)])
        # PBRT resets the camera transform at WorldBegin. No object transform
        # may leak into these world-space light declarations.
        self.assertNotRegex(world,r'(?m)^\s*(?:Translate|Transform|ConcatTransform|Scale|Rotate|LookAt)\b')

    def test_neutral_normal_is_linear(self):
        d, scene = self.export('normal')
        name = re.search(r'"string normalmap" "([^"]+)"', scene)[1]
        self.assertEqual(Path(name).suffix, '.pfm')
        w,h,p = read_pfm(d/name)
        self.assertEqual((w,h),(4,4))
        self.assertAlmostEqual(p[0],128/255,places=6)
        self.assertAlmostEqual(p[1],128/255,places=6)
        self.assertGreater(p[2],0.99)

    def test_normal_crops_partial_bc_blocks(self):
        d, scene = self.export('cropped_normal')
        name = re.search(r'"string normalmap" "([^"]+)"', scene)[1]
        self.assertEqual(Path(name).suffix, '.pfm')
        w,h,p = read_pfm(d/name)
        self.assertEqual((w,h,len(p)),(3,2,18))

    def test_missing_color_uses_constant_without_dangling_reference(self):
        _, scene = self.export('missing_color')
        self.assertNotIn('"texture reflectance"',scene)
        self.assertIn('"rgb reflectance" [ 0.400000 0.500000 0.600000 ]',scene)

    def test_constant_roughness_matches_engine_alpha(self):
        _, scene = self.export('rough_constant')
        self.assertIn('"float uroughness" [ 0.250000 ]',scene)
        self.assertIn('"bool remaproughness" [false]',scene)

    def test_roughness_map_decodes_srgb_before_squaring(self):
        d, scene = self.export('rough_texture')
        _,_,p = self.texture(d,scene,'roughness_0')
        self.assertAlmostEqual(p[0],0.04898,delta=0.0001)
        self.assertIn('"bool remaproughness" [false]',scene)

    def test_alpha_mask_is_bound_to_shape(self):
        d, scene = self.export('mask')
        self.assertIn('"texture alpha" "alpha_0"',scene)
        _,_,p = self.texture(d,scene,'alpha_0')
        self.assertEqual(p[:4],[0,1,1,1])

    def test_fractional_opacity_is_preserved_as_coverage(self):
        d, scene = self.export('coverage')
        _,_,p = self.texture(d,scene,'alpha_0')
        self.assertEqual(p[:4],[0,0.5,0.5,0.5])

    def test_shared_image_with_different_opacity_has_distinct_masks(self):
        d, scene = self.export('shared_opacity')
        _,_,a = self.texture(d,scene,'alpha_0')
        _,_,b = self.texture(d,scene,'alpha_1')
        self.assertAlmostEqual(a[1],0.2,places=6)
        self.assertAlmostEqual(b[1],0.8,places=6)

    def test_missing_mask_falls_back_to_thresholded_constant(self):
        _, scene = self.export('missing_mask')
        self.assertIn('"float alpha" [ 0.000000 ]',scene)

    def test_emissive_constant_is_a_scoped_area_light(self):
        _, scene = self.export('emissive')
        self.assertRegex(scene,r'(?s)AttributeBegin.*AreaLightSource "diffuse".*Shape "plymesh".*AttributeEnd')
        self.assertIn('"rgb L" [ 1.000000 0.500000 0.250000 ]',scene)
        self.assertIn('"float scale" [ 2.000000 ]',scene)

    def test_emissive_texture_uses_filename_not_texture_L(self):
        d, scene = self.export('emissive_texture')
        self.assertRegex(scene,r'AreaLightSource "diffuse"\s+"string filename" "[^"]+"')
        self.assertNotIn('"texture L"',scene)

    def test_missing_emissive_texture_falls_back_to_emissive_color(self):
        _, scene = self.export('missing_emissive')
        self.assertIn('AreaLightSource "diffuse"',scene)
        self.assertIn('"rgb L" [ 1.000000 0.000000 0.000000 ]',scene)

    def test_ply_failure_is_not_reported_as_success(self):
        d,_ = self.export('ply_failure',expected='failure')
        self.assertEqual((d/'result.txt').read_text(),'failure')

    def test_no_geometry_is_not_reported_as_success(self):
        d,_ = self.export('no_geometry',expected='failure')
        self.assertEqual((d/'result.txt').read_text(),'failure')

    def test_main_scene_open_failure_is_reported(self):
        d,_ = self.export('scene_failure',expected='failure')
        self.assertEqual((d/'result.txt').read_text(),'failure')

    def test_invalid_geometry_is_rejected_before_buffer_access(self):
        for mode in ('null_mesh', 'missing_materials', 'invalid_index_range',
                     'incomplete_triangle', 'invalid_index_type',
                     'invalid_vertex_index', 'empty_mesh'):
            with self.subTest(mode=mode):
                d,_ = self.export(mode,expected='failure')
                self.assertEqual((d/'result.txt').read_text(),'failure')

    def test_valid_16bit_indices_export_the_expected_face(self):
        for mode in ('indices16','indices16_bytes','indices32_bytes'):
            with self.subTest(mode=mode):
                d,_ = self.export(mode)
                self.assertEqual((d/'mesh_0.ply').read_bytes()[-13:],struct.pack('<Biii',3,0,1,2))

    def test_emission_does_not_leak_to_the_next_mesh(self):
        _,scene = self.export('emissive_isolation')
        scopes=re.findall(r'AttributeBegin(.*?)AttributeEnd',scene,re.S)
        self.assertEqual(len(scopes),2)
        self.assertIn('AreaLightSource',scopes[0])
        self.assertNotIn('AreaLightSource',scopes[1])

    def test_cutout_cache_preserves_opacities_on_opposite_sides_of_threshold(self):
        d,scene = self.export('close_opacities')
        _,_,a = self.texture(d,scene,'alpha_0')
        _,_,b = self.texture(d,scene,'alpha_1')
        self.assertEqual(a[1],0)
        self.assertEqual(b[1],1)

    def test_distinct_texture_hashes_do_not_overwrite_same_basename(self):
        d,scene = self.export('same_basename')
        names=re.findall(r'Texture "color_\d+"[^\n]*\n\s*"string filename" "([^"]+)"',scene)
        self.assertEqual(len(set(names)),2)
        self.assertNotEqual((d/names[0]).read_bytes(),(d/names[1]).read_bytes())

    def test_invalid_texture_metadata_falls_back_without_bogus_images(self):
        for mode in ('unsupported_format','zero_texture_size','invalid_stream_entry'):
            with self.subTest(mode=mode):
                d,scene = self.export(mode)
                self.assertEqual((d/'result.txt').read_text(),'success')
                self.assertNotIn('"texture reflectance"',scene)
                self.assertNotIn('"string normalmap"',scene)

    def test_corrupt_mip_falls_back_without_terminating_the_export(self):
        for mode in ('truncated_mip','bad_lzfse','wrong_decoded_size',
                     'mip_out_of_range','texture_out_of_range',
                     'bad_compression_mode','bad_compressed_length','decoded_overflow'):
            with self.subTest(mode=mode):
                _,scene=self.export(mode)
                self.assertNotIn('"texture reflectance"',scene)
                self.assertNotIn('"string normalmap"',scene)

    def test_corrupt_finest_mip_uses_valid_coarser_mip(self):
        d,scene=self.export('coarser_mip')
        self.assertIn('"texture reflectance"',scene)
        name=re.search(r'"string normalmap" "([^"]+)"',scene)[1]
        w,h,pixels=read_pfm(d/name)
        self.assertEqual((w,h),(2,2))
        self.assertTrue(all(math.isfinite(v) for v in pixels))


if __name__ == '__main__':
    unittest.main(verbosity=2)
