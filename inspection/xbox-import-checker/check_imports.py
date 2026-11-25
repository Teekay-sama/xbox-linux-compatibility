"""Inspect PE imports and Xbox package entry declarations without loading code."""

import argparse
from collections import Counter
import hashlib
import json
import mmap
import os
from pathlib import Path, PureWindowsPath
import struct
import sys
from xml.etree import ElementTree


MACHINES = {0x014C: 'x86', 0x8664: 'x64', 0xAA64: 'ARM64', 0xA641: 'ARM64EC'}
XBOX_NAMES = {
    'kernelx.dll', 'd3d12_xs.dll', 'xg_xs.dll', 'xmem.dll', 'xcrdapi.dll',
    'acphal.dll', 'xapu.dll', 'chad.dll', 'bink2scarlett.dll', 'xboxbvhbuilder.dll',
}
WINDOWS_NAMES = {
    'ntdll.dll', 'kernel32.dll', 'kernelbase.dll', 'user32.dll', 'gdi32.dll',
    'advapi32.dll', 'ole32.dll', 'oleaut32.dll', 'combase.dll', 'shell32.dll',
    'd3d11.dll', 'd3d12.dll', 'dxgi.dll', 'dxcore.dll', 'ucrtbase.dll', 'msvcrt.dll',
    'ws2_32.dll', 'winhttp.dll', 'bcrypt.dll', 'crypt32.dll', 'xaudio2_9.dll',
}
MAX_DESCRIPTORS = 10_000
MAX_THUNKS = 100_000
MAX_IMPORTS = 200_000


class FormatError(ValueError):
    pass


class PE:
    def __init__(self, data):
        self.data = data
        if len(data) < 64 or data[:2] != b'MZ':
            raise FormatError('missing DOS header')
        pe = self.read('<I', 0x3C)[0]
        if pe < 64 or data[pe:pe + 4] != b'PE\0\0':
            raise FormatError('missing PE signature')
        self.machine, section_count, _, _, _, opt_size, self.flags = self.read('<HHIIIHH', pe + 4)
        opt = pe + 24
        magic = self.read('<H', opt)[0]
        if magic not in (0x10B, 0x20B):
            raise FormatError(f'unsupported optional header magic {magic:#x}')
        self.bits = 64 if magic == 0x20B else 32
        dir_offset = 112 if self.bits == 64 else 96
        if opt_size < dir_offset or opt + opt_size > len(data):
            raise FormatError('truncated optional header')
        self.image_base = self.read('<Q', opt + 24)[0] if self.bits == 64 else self.read('<I', opt + 28)[0]
        self.entry_rva = self.read('<I', opt + 16)[0]
        self.header_size = self.read('<I', opt + 60)[0]
        self.subsystem = self.read('<H', opt + 68)[0]
        directory_count = self.read('<I', opt + dir_offset - 4)[0]
        if directory_count > (opt_size - dir_offset) // 8:
            raise FormatError('data directories exceed the optional header')
        self.directories = [self.read('<II', opt + dir_offset + i * 8) for i in range(directory_count)]
        self.sections = []
        section_table = opt + opt_size
        if section_table + section_count * 40 > len(data):
            raise FormatError('truncated section table')
        for i in range(section_count):
            virtual_size, rva, raw_size, offset = self.read('<IIII', section_table + i * 40 + 8)
            self.sections.append((rva, max(virtual_size, raw_size), offset, raw_size))

    def read(self, fmt, offset):
        if offset < 0 or offset + struct.calcsize(fmt) > len(self.data):
            raise FormatError(f'truncated file at offset {offset:#x}')
        return struct.unpack_from(fmt, self.data, offset)

    def mapped(self, rva):
        if 0 <= rva < min(self.header_size, len(self.data)):
            return rva, min(self.header_size, len(self.data)) - rva
        for start, size, raw_offset, raw_size in self.sections:
            if start <= rva < start + size:
                delta = rva - start
                offset = raw_offset + delta
                available = min(raw_size - delta, len(self.data) - offset)
                if available > 0:
                    return offset, available
                break
        raise FormatError(f'unmapped file RVA {rva:#x}')

    def c_string(self, rva):
        offset, available = self.mapped(rva)
        end = self.data.find(b'\0', offset, offset + min(available, 4096))
        if end <= offset:
            raise FormatError(f'missing or unterminated import name at RVA {rva:#x}')
        return self.data[offset:end].decode('ascii', errors='backslashreplace')

    def directory(self, index):
        return self.directories[index] if index < len(self.directories) else (0, 0)

    def thunk_symbols(self, rva, legacy_va=False):
        offset, available = self.mapped(rva)
        width = self.bits // 8
        ordinal_flag = 1 << (self.bits - 1)
        fmt = '<Q' if self.bits == 64 else '<I'
        symbols = []
        for index in range(min(available // width, MAX_THUNKS)):
            value = self.read(fmt, offset + index * width)[0]
            if not value:
                return symbols
            if value & ordinal_flag:
                if value & ~(ordinal_flag | 0xFFFF):
                    raise FormatError(f'invalid ordinal thunk at RVA {rva + index * width:#x}')
                symbols.append({'ordinal': value & 0xFFFF})
            else:
                name_rva = value - self.image_base if legacy_va else value
                if not 0 < name_rva <= 0xFFFFFFFD:
                    raise FormatError(f'invalid hint/name RVA {name_rva:#x}')
                hint_offset, hint_available = self.mapped(name_rva)
                if hint_available < 3:
                    raise FormatError(f'truncated import hint/name at RVA {name_rva:#x}')
                symbols.append({'name': self.c_string(name_rva + 2), 'hint': self.read('<H', hint_offset)[0]})
        raise FormatError(f'import thunk table at RVA {rva:#x} is unterminated or exceeds limits')

    def imports(self, delayed=False):
        rva, declared_size = self.directory(13 if delayed else 1)
        if not rva:
            return []
        offset, available = self.mapped(rva)
        fmt = '<8I' if delayed else '<5I'
        stride = struct.calcsize(fmt)
        limit = min(available, declared_size) if declared_size else available
        results = []
        total_symbols = 0
        for i in range(min(limit // stride, MAX_DESCRIPTORS)):
            fields = self.read(fmt, offset + i * stride)
            if not any(fields):
                return results
            if delayed:
                flags, name, _, iat, lookup, _, _, timestamp = fields
                if flags not in (0, 1):
                    raise FormatError(f'unknown delay import flags {flags:#x}')
                legacy_va = flags == 0
            else:
                lookup, timestamp, _, name, iat = fields
                legacy_va = False
            base = self.image_base if legacy_va else 0
            result = {'module': self.c_string(name - base), 'kind': 'delay' if delayed else 'normal'}
            if not lookup and timestamp:
                result.update(symbols=None, note='Bound IAT with no separate import lookup table.')
            else:
                thunk = lookup or iat
                if not thunk:
                    raise FormatError(f'missing import lookup table for {result["module"]}')
                result['symbols'] = self.thunk_symbols(thunk - base, legacy_va)
                total_symbols += len(result['symbols'])
                if total_symbols > MAX_IMPORTS:
                    raise FormatError('total import symbol count exceeds limits')
            results.append(result)
        raise FormatError('import descriptor table is unterminated or exceeds limits')


def basename(module):
    path = PureWindowsPath(module)
    return path.name == module and not path.drive and ':' not in module and module not in ('', '.', '..')


def providers(entry, package):
    matches = {}
    for directory in dict.fromkeys((entry.parent, package)):
        for path in directory.iterdir():
            if path.suffix.casefold() == '.dll' and path.is_file():
                matches.setdefault(path.name.casefold(), path)
    return matches


def classify(record, local, pe):
    module = record['module']
    canonical = module.casefold()
    xbox_hint = canonical in XBOX_NAMES or canonical.endswith(('_xbox.dll', '_xs.dll', 'scarlett.dll'))
    record['xbox_name_hint'] = xbox_hint
    if not basename(module):
        record['classification'] = 'other_path'
    elif canonical in local:
        record.update(classification='package_local_candidate', provider=str(local[canonical]))
    elif xbox_hint:
        record['classification'] = 'xbox_specific_candidate'
    elif canonical.startswith(('api-ms-win-', 'ext-ms-win-')) and canonical.endswith('.dll'):
        record['classification'] = 'windows_api_set_candidate'
    else:
        system_root = os.environ.get('SystemRoot') if os.name == 'nt' else None
        folder = 'SysWOW64' if pe.bits == 32 and system_root and (Path(system_root) / 'SysWOW64').is_dir() else 'System32'
        system = Path(system_root) / folder / module if system_root else None
        if system and system.is_file():
            record.update(classification='windows_system_candidate', provider=str(system))
        elif canonical in WINDOWS_NAMES:
            record['classification'] = 'windows_system_candidate'
        else:
            record['classification'] = 'unknown_provider'
    return record


def inspect_image(path, package):
    with path.open('rb') as stream:
        if not os.fstat(stream.fileno()).st_size:
            raise FormatError('empty file')
        with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
            pe = PE(data)
            local = providers(path, package)
            records = [classify(record, local, pe) for record in pe.imports() + pe.imports(delayed=True)]
            return {
                'path': str(path),
                'sha256': hashlib.sha256(data).hexdigest(),
                'size_bytes': len(data),
                'architecture': MACHINES.get(pe.machine, f'unknown ({pe.machine:#06x})'),
                'pe_bits': pe.bits,
                'image_base': f'{pe.image_base:#x}',
                'entry_rva': f'{pe.entry_rva:#x}',
                'subsystem': pe.subsystem,
                'is_dll': bool(pe.flags & 0x2000),
                'has_tls_directory': bool(pe.directory(9)[0]),
                'has_load_config_directory': bool(pe.directory(10)[0]),
                'import_module_counts': dict(Counter(record['classification'] for record in records)),
                'imports': records,
            }


def config_entries(folder):
    config = next((file for file in folder.iterdir() if file.name.casefold() == 'microsoftgame.config' and file.is_file()), None)
    if config is None:
        return None, []
    with config.open('rb') as stream:
        data = stream.read(1024 * 1024 + 1)
    if len(data) > 1024 * 1024:
        raise FormatError('MicrosoftGame.Config exceeds 1 MiB')
    root = ElementTree.fromstring(data)
    entries = []
    for element in root.iter():
        if element.tag.rsplit('}', 1)[-1] != 'Executable':
            continue
        name = element.get('Name', '')
        relative = PureWindowsPath(name)
        if not name or relative.anchor or '..' in relative.parts or relative.suffix.casefold() != '.exe':
            entries.append({'name': name, 'error': 'Expected a relative .exe path inside the package.'})
            continue
        path = folder.joinpath(*relative.parts).resolve()
        entry = {'name': name, 'target_device_family': element.get('TargetDeviceFamily')}
        if path.is_relative_to(folder) and path.is_file():
            entry['path'] = str(path)
        else:
            entry['error'] = 'Declared executable is missing or is outside the package.'
        entries.append(entry)
    return config, entries


def inspect_target(target):
    folder = target if target.is_dir() else target.parent
    config = None
    entries = []
    errors = []
    try:
        config, entries = config_entries(folder)
    except (ElementTree.ParseError, OSError, FormatError) as error:
        errors.append(f'MicrosoftGame.Config: {error}')
    if target.is_file():
        targets = [target]
    elif entries:
        targets = list(dict.fromkeys(Path(entry['path']) for entry in entries if 'path' in entry))
    else:
        targets = sorted(path.resolve() for path in folder.iterdir()
                         if path.suffix.casefold() == '.exe' and path.is_file())
        targets = [path for path in targets if path.is_relative_to(folder)]
    if not targets:
        errors.append('No executable to inspect. Supply an unpacked package or a path to the executable.')

    images = []
    for path in targets:
        try:
            images.append(inspect_image(path, folder))
        except (FormatError, OSError) as error:
            errors.append(f'{path}: {error}')
    return {
        'input': str(target),
        'package_directory': str(folder),
        'config_path': str(config) if config else None,
        'declared_executables': entries,
        'images': images,
        'errors': errors,
        'scope': 'Static PE import and provider-name inspection. Dynamic loads, export compatibility and startup behavior require separate checks.',
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('target', type=Path, help='PE image or directory containing an unpacked package')
    options = parser.parse_args()
    target = options.target.resolve()
    if not target.exists():
        parser.error(f'Path does not exist: {target}')
    try:
        report = inspect_target(target)
    except OSError as error:
        parser.error(str(error))
    print(json.dumps(report, indent=2))
    return 1 if report['errors'] or (target.is_dir() and any('error' in item for item in report['declared_executables'])) else 0


if __name__ == '__main__':
    sys.exit(main())
