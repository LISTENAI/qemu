#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Test asset validation without embedding vendor binaries in this repository."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    'lisem_assets', Path(__file__).resolve().parents[2] / 'scripts/lisem-assets.py')
assets = importlib.util.module_from_spec(spec)
spec.loader.exec_module(assets)


class AssetTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)

    def write_roms(self):
        manifest = {}
        for name, size, address in (('ap', 65536, 0), ('cp', 32768, 0x200000)):
            data = bytes(size)
            (self.directory / (name + '.bin')).write_bytes(data)
            manifest[name] = dict(file=name + '.bin', size=size, address=address,
                                  sha256=hashlib.sha256(data).hexdigest())
        (self.directory / 'manifest.json').write_text(json.dumps(manifest))
        return manifest

    def test_rom_layout_and_integrity(self):
        self.write_roms()
        header = assets.rom_header(self.directory)
        self.assertIn('"arcs-ap-rom", 0x0', header)
        self.assertIn('"arcs-cp-rom", 0x200000', header)
        with (self.directory / 'cp.bin').open('r+b') as stream:
            stream.write(b'X')
        with self.assertRaises(ValueError):
            assets.rom_header(self.directory)

    def test_rom_fixed_manifest_contract(self):
        manifest = self.write_roms()
        for field, value in (('size', 32768), ('address', 4), ('file', '../ap.bin')):
            changed = json.loads(json.dumps(manifest))
            changed['ap'][field] = value
            (self.directory / 'manifest.json').write_text(json.dumps(changed))
            with self.assertRaises(ValueError):
                assets.rom_header(self.directory)
        manifest['extra'] = {}
        (self.directory / 'manifest.json').write_text(json.dumps(manifest))
        with self.assertRaises(ValueError):
            assets.rom_header(self.directory)

    def test_luna_target_abi_and_content(self):
        api = self.directory / 'luna.h'
        api.write_bytes(b'API fixture')
        archive = self.directory / 'liblisem-luna.a'
        archive.write_bytes(b'Archive fixture')
        manifest = dict(abi=1, target='darwin-aarch64', file=archive.name,
                        sha256=assets.digest(archive), api_sha256=assets.digest(api))
        (self.directory / 'manifest.json').write_text(json.dumps(manifest))
        self.assertIn(manifest['sha256'], assets.luna_header(
            self.directory, api, 'darwin-aarch64'))
        for field, value in (('abi', 2), ('target', 'linux-aarch64'),
                             ('file', 'other.a'), ('sha256', 'bad'),
                             ('api_sha256', 'bad')):
            changed = dict(manifest, **{field: value})
            (self.directory / 'manifest.json').write_text(json.dumps(changed))
            with self.assertRaises(ValueError):
                assets.luna_header(self.directory, api, 'darwin-aarch64')


if __name__ == '__main__':
    unittest.main()
