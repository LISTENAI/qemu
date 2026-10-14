#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate externally supplied Lisem assets and generate build-only headers."""

import argparse
import hashlib
import json
from pathlib import Path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def rom_header(directory):
    manifest = json.loads((directory / 'manifest.json').read_text())
    if set(manifest) != {'ap', 'cp'}:
        raise ValueError('Unexpected ARCS ROM manifest entries')
    images = {}
    for name, size, address in (('ap', 65536, 0), ('cp', 32768, 0x200000)):
        entry = manifest[name]
        path = directory / (name + '.bin')
        if (entry['file'] != path.name or entry['size'] != size
                or entry['address'] != address or path.stat().st_size != size
                or digest(path) != entry['sha256']):
            raise ValueError('ARCS %s ROM does not match its manifest' % name.upper())
        images[name] = path.read_bytes()
    lines = ['/* Generated from the fixed, hash-verified ARCS mask ROM assets. */',
             'typedef struct ArcsROMImage {',
             '    const char *name; uint32_t address, size; const uint8_t *bytes; const char *sha256;',
             '} ArcsROMImage;']
    for name in ('ap', 'cp'):
        data = images[name]
        lines.append('static const uint8_t arcs_%s_rom[] = {' % name)
        lines.extend('    ' + ','.join('0x%02x' % b for b in data[i:i + 16]) + ','
                     for i in range(0, len(data), 16))
        lines.append('};')
    lines.append('static const ArcsROMImage arcs_rom_images[] = {')
    for name in ('ap', 'cp'):
        lines.append('    {"arcs-%s-rom", 0x%x, sizeof(arcs_%s_rom), arcs_%s_rom, "%s"},' %
                     (name, manifest[name]['address'], name, name, manifest[name]['sha256']))
    lines.append('};\n')
    return '\n'.join(lines)


def venusa_rom_header(directory):
    manifest = json.loads((directory / 'manifest.json').read_text())
    expected = '6b799d7e2c4c42f1e39760d4ba413bc3560ee81350754cd395f205e27209ac1b'
    path = directory / 'ap.bin'
    if (set(manifest) != {'ap'} or manifest['ap'] != {
            'file': 'ap.bin', 'size': 32768, 'address': 0, 'sha256': expected}
            or path.stat().st_size != 32768 or digest(path) != expected):
        raise ValueError('VenusA mask ROM does not match its fixed identity')
    data = path.read_bytes()
    lines = ['/* Generated from the fixed, hash-verified VenusA mask ROM. */',
             'static const uint8_t venusa_rom_bytes[] = {']
    lines.extend('    ' + ','.join('0x%02x' % b for b in data[i:i + 16]) + ','
                 for i in range(0, len(data), 16))
    lines.append('};\n')
    return '\n'.join(lines)


def luna_header(directory, api, target):
    manifest = json.loads((directory / 'manifest.json').read_text())
    archive = directory / 'liblisem-luna.a'
    if (manifest.get('abi') != 1 or manifest.get('target') != target
            or manifest.get('file') != archive.name
            or manifest.get('sha256') != digest(archive)
            or manifest.get('api_sha256') != digest(api)):
        raise ValueError('LUNA backend does not match its target, ABI or manifest')
    # Archive changes invalidate the including object and therefore its link.
    return ('/* Validated LUNA ABI 1; archive SHA-256: %s */\n' %
            manifest['sha256'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kind', choices=('rom', 'venusa-rom', 'luna'))
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--api', type=Path)
    parser.add_argument('--target')
    args = parser.parse_args()
    if args.kind == 'luna' and (not args.api or not args.target):
        parser.error('luna requires --api and --target')
    content = (rom_header(args.directory) if args.kind == 'rom' else
               venusa_rom_header(args.directory) if args.kind == 'venusa-rom' else
               luna_header(args.directory, args.api, args.target))
    args.output.write_text(content)


if __name__ == '__main__':
    main()
