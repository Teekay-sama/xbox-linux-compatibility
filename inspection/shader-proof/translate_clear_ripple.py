"""Extract and lower a straight-line Xbox pixel shader from the local archive."""

import hashlib
import json
from pathlib import Path
import struct
import sys

HERE = Path(__file__).resolve().parent
INSPECTION = HERE.parent
sys.path.insert(0, str(INSPECTION))

from decode_xbox_shaders import CODE_END, decode
from inspect_shaders import dxbc_at
from probe_sgd5 import inflate


ARCHIVE_OFFSET = 0x5047C04
SHADER_NAME = 'PS_clearRipple_57383fac_Wrapped'
INLINE_FLOATS = {0x80: 0.0, 0xF0: 0.5, 0xF2: 1.0}


def extract_shader():
    manifest = json.loads((INSPECTION / 'sgd5-report.json').read_text(encoding='utf-8'))
    record = next(
        record for group in manifest['groups'] for stage in group['stage_arrays']
        for record in stage['records'] if record['payload_offset'] == ARCHIVE_OFFSET
    )
    if record['name'] != SHADER_NAME:
        raise ValueError('Archive record name differs from the selected shader')
    candidate = next(c for c in manifest['compressed_candidates'] if c['offset'] == ARCHIVE_OFFSET)
    with (INSPECTION.parent / 'shaders.rpf').open('rb') as archive:
        archive.seek(ARCHIVE_OFFSET)
        magic, packed_size, output_size = struct.unpack('<III', archive.read(12))
        if (magic, packed_size, output_size) != (
            0xDDCE7, 12 + candidate['compressed_bytes'], candidate['decompressed_bytes']
        ):
            raise ValueError('Archive wrapper differs from the manifest')
        raw = inflate(archive.read(packed_size - 12), output_size)
    if hashlib.sha256(raw).hexdigest() != candidate['sha256']:
        raise ValueError('Shader container hash differs from the manifest')
    info, payloads = decode(raw)
    if info['program_kind'] != 0 or len(payloads) != 1:
        raise ValueError('Expected one pixel shader payload')
    chunks = {part['tag']: part for part in dxbc_at(raw, 0)['parts']}
    hash_chunk = chunks['HASH']
    if hash_chunk['size'] != 20:
        raise ValueError('Unexpected shader HASH size')
    target = chunks['OSG1']
    signature = raw[target['offset'] + 8:target['offset'] + 8 + target['size']]
    if len(signature) < 40 or struct.unpack_from('<II', signature) != (1, 8):
        raise ValueError('Expected one output signature element')
    _, name_offset, index, _, component_type, slot, mask = struct.unpack_from('<6IB', signature, 8)
    if (index, component_type, slot, mask) != (0, 3, 0, 15) or (
        signature[name_offset:].split(b'\0', 1)[0] != b'SV_Target'
    ):
        raise ValueError('Expected full float4 SV_Target0')
    hash_start = hash_chunk['offset'] + 12
    return payloads[0][1], raw[hash_start:hash_start + 16]


def lower(code, shader_hash):
    """Constant-fold this bounded instruction subset and record every instruction."""
    if len(code) < 24 or len(code) % 4:
        raise ValueError('Expected a complete word-aligned native shader')
    words = struct.unpack(f'<{len(code) // 4}I', code)
    listing = []

    def emit(index, count, operation):
        encoding = ' '.join(f'{word:08x}' for word in words[index:index + count])
        listing.append(f'{index * 4:04x}  {encoding:26}  {operation}')

    # Xbox stores the HASH digest in two literal-bearing VOP3 v_nop instructions.
    embedded_hash = bytearray()
    for index in (0, 3):
        first, sources, literal = words[index:index + 3]
        if first >> 16 != 0xD580 or (sources >> 18) & 0x1FF != 0xFF:
            raise ValueError('Unexpected Xbox shader hash preamble')
        embedded_hash.extend(struct.pack('<HBBI', first & 0xFFFF, sources & 255,
                                         (sources >> 9) & 255, literal))
        emit(index, 3, 'v_nop (Xbox hash preamble with literal)')
    if embedded_hash != shader_hash:
        raise ValueError('Xbox shader hash preamble differs from HASH chunk')

    registers = {}
    color = None
    cursor = 6
    terminated = False
    while cursor < len(words):
        word = words[cursor]
        count = 1
        if word == 0xBFA00003:
            operation = 's_inst_prefetch 3'
        elif word == 0xBF800000:
            operation = 's_nop 0'
        elif word == 0xBF810000:
            emit(cursor, 1, 's_endpgm')
            cursor += 1
            terminated = True
            break
        elif word & 0xFE01FE00 == 0x7E000200:
            dest, source = (word >> 17) & 255, word & 511
            if source not in INLINE_FLOATS:
                raise ValueError(f'Unsupported v_mov_b32 source {source:#x}')
            registers[dest] = INLINE_FLOATS[source]
            operation = f'v_mov_b32 v{dest}, {registers[dest]}'
        elif word >> 25 == 0x2F:
            dest, high, low = (word >> 17) & 255, (word >> 9) & 255, (word & 511) - 256
            low_value, high_value = registers.get(low), registers.get(high)
            if not isinstance(low_value, float) or not isinstance(high_value, float):
                raise ValueError('Packed conversion needs two known float source registers')
            packed = struct.pack('<2e', low_value, high_value)
            if struct.unpack('<2e', packed) != (low_value, high_value):
                raise ValueError('Rounding is outside this decoder subset')
            registers[dest] = packed
            operation = f'v_cvt_pkrtz_f16_f32 v{dest}, v{low}, v{high}'
        elif word == 0xF8001C0F:
            if color is not None or cursor + 1 >= len(words) or words[cursor + 1] >> 16:
                raise ValueError('Expected one full compressed MRT0 export')
            sources = words[cursor + 1]
            rg, ba = sources & 255, (sources >> 8) & 255
            if not all(isinstance(registers.get(reg), bytes) for reg in (rg, ba)):
                raise ValueError('Export needs two known packed float pairs')
            color = struct.unpack('<4e', registers[rg] + registers[ba])
            operation = f'exp mrt0, v{rg}, v{ba}, compr done vm, rgba'
            count = 2
        else:
            raise ValueError(f'Unsupported instruction {word:08x} at {cursor * 4:#x}')
        emit(cursor, count, operation)
        cursor += count

    padding = code[cursor * 4:]
    if not terminated or color is None or len(padding) < 20 or padding != CODE_END * (len(padding) // 4):
        raise ValueError('Expected an export, termination, and AMD end padding')
    listing.append(f'{cursor * 4:04x}  bf9f0000 × {len(padding) // 4:<14}  s_code_end padding')
    return color, listing


def main():
    code, shader_hash = extract_shader()
    color, listing = lower(code, shader_hash)
    digest = hashlib.sha256(code).hexdigest()
    values = ', '.join(f'{value:.1f}' for value in color)
    hlsl = f'// Generated from {SHADER_NAME} at archive offset {ARCHIVE_OFFSET:#x}.\n'
    hlsl += f'float4 main() : SV_Target0\n{{\n    return float4({values});\n}}\n'
    (HERE / 'clear_ripple.hlsl').write_text(hlsl, encoding='utf-8')
    header = f'{SHADER_NAME} at archive offset {ARCHIVE_OFFSET:#x}\nNative code SHA-256 {digest}\n\n'
    (HERE / 'clear_ripple.rdna2.txt').write_text(header + '\n'.join(listing) + '\n', encoding='utf-8')
    print(f'Lowered {len(listing) - 1} instructions. RGBA = {color}.')


if __name__ == '__main__':
    main()
