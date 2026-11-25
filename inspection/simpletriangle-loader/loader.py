"""Inspect a Scarlett build and exercise the desktop startup and frame pipeline."""

import argparse
from collections import deque
import json
import mmap
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

from exports import read_exports

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'xbox-import-checker'))
import check_imports as checker

FIXTURE_BIN = HERE.parent / 'xbox-style-triangle' / 'bin'
FIXTURE_EXE = 'xbox_style_triangle.exe'
FIXTURE_SHIM = 'xbox_style_graphics.dll'
SHIM_FIXTURE_BIN = HERE / 'shims' / 'bin'
SHIM_FIXTURE_EXE = 'simpletriangle_shim_test.exe'


def exports_for(path):
    with path.open('rb') as file:
        if not path.stat().st_size:
            raise checker.FormatError('empty DLL file')
        with mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
            pe = checker.PE(data)
            if not pe.flags & 0x2000:
                raise checker.FormatError('provider is not marked as a PE DLL')
            return pe.machine, read_exports(pe)


def verify_binding(record, path, export_cache, machine):
    if path not in export_cache:
        try:
            export_cache[path] = exports_for(path)
        except (OSError, checker.FormatError, ValueError, struct.error) as error:
            export_cache[path] = str(error)
    provider = export_cache[path]
    record['provider'] = str(path)
    if isinstance(provider, str):
        record.update(state='invalid_provider', error=provider)
        return
    provider_machine, exports = provider
    if provider_machine != machine:
        record.update(state='architecture_mismatch', provider_machine=f'{provider_machine:#06x}')
        return
    if record['symbols'] is None:
        record['state'] = 'import_symbols_unavailable'
        return
    missing = []
    forwarded = []
    for symbol in record['symbols']:
        if 'name' in symbol:
            label = symbol['name']
            ordinal = exports['names'].get(label)
        else:
            ordinal = symbol['ordinal']
            label = f'#{ordinal}'
        if ordinal not in exports['ordinals']:
            missing.append(label)
        elif exports['ordinals'][ordinal] is not None:
            forwarded.append({'symbol': label, 'target': exports['ordinals'][ordinal]})
    record['missing_symbols'] = missing
    record['forwarded_symbols'] = forwarded
    if missing:
        record['state'] = 'missing_exports'
    else:
        record['state'] = 'exports_found'


def preflight(target, shim_dir=None):
    intake = checker.inspect_target(target)
    folder = Path(intake['package_directory'])
    shim_paths = {} if shim_dir is None else checker.providers(shim_dir / '_', shim_dir)
    failures = list(intake['errors'])
    if target.is_dir():
        failures.extend(f'{entry["name"]}: {entry["error"]}' for entry in intake['declared_executables'] if 'error' in entry)
    pending = deque((image, True) for image in intake['images'])
    queued = {Path(image['path']).resolve() for image in intake['images']}
    visited = set()
    export_cache = {}
    images = []
    while pending:
        image, entry = pending.popleft()
        path = Path(image['path']).resolve()
        if path in visited:
            continue
        visited.add(path)
        if entry and (image['architecture'] != 'x64' or image['is_dll'] or image['entry_rva'] == '0x0'):
            failures.append(f'{path.name}: expected an x64 EXE with a PE entry point.')
        if entry and path.name.casefold() == 'simpletriangle.exe':
            for shader in ('VertexShader.cso', 'PixelShader.cso'):
                if not any((directory / shader).is_file() for directory in (path.parent, folder)):
                    failures.append(f'{path.name}: missing frame asset {shader}.')
        machine = next((number for number, name in checker.MACHINES.items() if name == image['architecture']), None)
        bindings = []
        for item in image['imports']:
            record = dict(item)
            module = item['module']
            category = item['classification']
            provider = None
            if category == 'package_local_candidate':
                provider = Path(item['provider']).resolve()
                record['route'] = 'package_dll'
            elif category == 'windows_system_candidate' and item.get('provider'):
                provider = Path(item['provider']).resolve()
                record['route'] = 'windows_dll'
            elif category in ('xbox_specific_candidate', 'unknown_provider') and module.casefold() in shim_paths:
                provider = shim_paths[module.casefold()].resolve()
                record['route'] = 'supplied_shim'
            elif category in ('windows_api_set_candidate', 'windows_system_candidate'):
                record.update(route='windows_loader', state='requires_host_resolution')
            else:
                record.update(route='unresolved', state='provider_not_found')
            if provider is not None:
                verify_binding(record, provider, export_cache, machine)
                if record['route'] in ('package_dll', 'supplied_shim') and record['state'] == 'exports_found' and provider not in queued:
                    queued.add(provider)
                    try:
                        pending.append((checker.inspect_image(provider, folder), False))
                    except (OSError, checker.FormatError, ValueError) as error:
                        failures.append(f'{provider.name}: {error}')
            bindings.append(record)
        images.append({**image, 'is_entry': entry, 'imports': bindings})

    blockers = []
    deferred = []
    windows_runtime = []
    for image in images:
        for record in image['imports']:
            description = {
                'image': Path(image['path']).name, 'module': record['module'],
                'state': record['state'], 'kind': record['kind'],
            }
            if record['state'] == 'requires_host_resolution' or record.get('forwarded_symbols'):
                windows_runtime.append(description)
            if record['state'] not in ('exports_found', 'requires_host_resolution'):
                (deferred if record['kind'] == 'delay' else blockers).append(description)
    return {
        'input': intake['input'], 'declared_executables': intake['declared_executables'],
        'errors': failures, 'blocking_imports': blockers, 'deferred_imports': deferred,
        'runtime_resolution_notes': windows_runtime, 'images': images,
        'static_preflight_passed': bool(images) and not failures and not blockers,
        'scope': 'Static import and export checks. ABI behavior, forwarder targets and dynamic imports need runtime validation.',
    }


def read_bmp(path):
    data = path.read_bytes()
    if len(data) < 54 or data[:2] != b'BM':
        raise ValueError(f'{path.name}: expected a BMP frame')
    offset, header, width, signed_height, planes, bpp, compression = struct.unpack_from('<IIiiHHI', data, 10)
    height = abs(signed_height)
    if header < 40 or offset < 14 + header or width <= 0 or height == 0 or planes != 1 or bpp != 24 or compression:
        raise ValueError(f'{path.name}: expected an uncompressed 24-bit BMP')
    stride = (width * 3 + 3) & ~3
    if offset + stride * height > len(data):
        raise ValueError(f'{path.name}: truncated BMP frame')
    rows = range(height - 1, -1, -1) if signed_height > 0 else range(height)
    pixels = b''.join(data[offset + row * stride:offset + row * stride + width * 3] for row in rows)
    return (width, height), pixels


def compare_frames(actual_path, reference_path, tolerance=1):
    dimensions, actual = read_bmp(actual_path)
    reference_dimensions, expected = read_bmp(reference_path)
    if dimensions != reference_dimensions:
        raise ValueError(f'Frame size {dimensions} differs from reference {reference_dimensions}')
    maximum = 0
    mismatches = 0
    for offset in range(0, len(actual), 3):
        difference = max(abs(actual[offset + c] - expected[offset + c]) for c in range(3))
        maximum = max(maximum, difference)
        mismatches += difference > tolerance
    return {
        'path': str(actual_path), 'reference': str(reference_path),
        'dimensions': dimensions, 'pixels': len(actual) // 3,
        'max_channel_delta': maximum, 'tolerance': tolerance,
        'mismatched_pixels': mismatches, 'passed': mismatches == 0,
    }


def fixture(adapter, with_shims=False):
    source = SHIM_FIXTURE_BIN if with_shims else FIXTURE_BIN
    exe = SHIM_FIXTURE_EXE if with_shims else FIXTURE_EXE
    libraries = ('startup_shim.dll', 'frame_shim.dll', FIXTURE_SHIM) if with_shims else (FIXTURE_SHIM,)
    required = (exe, *libraries)
    if any(not (source / name).is_file() for name in required):
        raise ValueError('Build the selected local Windows fixture before launching it.')
    output_root = HERE / 'runs'
    output_root.mkdir(exist_ok=True)
    prefix = f'shim-{adapter}-' if with_shims else f'{adapter}-'
    run = Path(tempfile.mkdtemp(prefix=prefix, dir=output_root))
    for name in required:
        shutil.copy2(source / name, run / name)
    description = 'local startup and frame shim prototype' if with_shims else 'local Windows SimpleTriangle reference'
    report = {'fixture': description,
              'adapter': adapter, 'preflight': preflight(run / exe), 'passed': False}
    report_path = run / 'report.json'
    try:
        if not report['preflight']['static_preflight_passed']:
            report['error'] = 'Fixture did not pass import preflight.'
            return report_path, report
        command = [str(run / exe)]
        if not with_shims:
            command.append('--microsoft')
        if adapter == 'warp':
            command.append('--warp')
        result = subprocess.run(command, cwd=run, capture_output=True, text=True,
                                errors='replace', timeout=45, check=False)
        report.update({'process_exit_code': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr})
        if result.returncode:
            report['error'] = 'Fixture process returned a failure status.'
            return report_path, report
        name = f'simpletriangle-reference-{adapter}.bmp'
        report['frame'] = compare_frames(run / name, FIXTURE_BIN / name)
        report['passed'] = report['frame']['passed']
        if not report['passed']:
            report['error'] = 'Rendered frame differs from the reference.'
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        report['error'] = str(error)
    finally:
        report_path.write_text(json.dumps(report, indent=2), encoding='utf-8')
    return report_path, report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    scan = commands.add_parser('preflight', help='read a real executable or unpacked build without running it')
    scan.add_argument('target', type=Path)
    scan.add_argument('--shim-dir', type=Path, help='directory of candidate replacement DLLs to inspect')
    smoke = commands.add_parser('fixture', help='launch only the local source-built reference fixture')
    smoke.add_argument('--adapter', choices=('intel', 'warp'), default='intel')
    shim_smoke = commands.add_parser('shim-fixture', help='launch the local startup and frame shim fixture')
    shim_smoke.add_argument('--adapter', choices=('intel', 'warp'), default='intel')
    args = parser.parse_args()
    try:
        if args.command == 'preflight':
            target = args.target.resolve(strict=True)
            shim_dir = args.shim_dir.resolve(strict=True) if args.shim_dir else None
            if shim_dir is not None and not shim_dir.is_dir():
                raise ValueError('--shim-dir must point to a directory')
            report = preflight(target, shim_dir)
            print(json.dumps(report, indent=2))
            return 0 if report['static_preflight_passed'] else 1
        path, report = fixture(args.adapter, with_shims=args.command == 'shim-fixture')
        if 'stdout' in report:
            print(report['stdout'], end='')
        if report.get('stderr'):
            print(report['stderr'], end='', file=sys.stderr)
        if report['passed']:
            frame = report['frame']
            print(f'{frame["pixels"]} frame pixels passed; max channel delta {frame["max_channel_delta"]}.')
        else:
            print(report.get('error', 'Fixture check failed.'), file=sys.stderr)
        print(f'Report: {path}')
        return 0 if report['passed'] else 1
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    sys.exit(main())
