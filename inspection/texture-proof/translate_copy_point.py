"""Offline translator for the verified PS_CopyPoint instruction sequence."""

import hashlib
from pathlib import Path
import struct
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

from decode_xbox_shaders import CODE_END, decode
from inspect_shaders import dxbc_at
from probe_sgd5 import inflate

ARCHIVE_OFFSET = 0x6E213DE
NAME = 'PS_CopyPoint_8eeb7a25_Wrapped'
CONTAINER_SHA256 = 'ec7952754a0d62be538e29e2f85a2844fa28101ddd1b186b556419422a8ff8dd'

NATIVE_CODE_SHA256 = "2736156166cb7b8f5ad73151bbc30f194ae055115e3d7d4f501ef8e280ba0496"

# Instruction widths and labels for the verified program. Encoding words are
# read from the caller's local data after verifying its digest.
BODY = (
    (1, 's_inst_prefetch 3'),
    (1, 's_mov_b64 s[12:13], exec'),
    (1, 's_wqm_b64 exec, exec'),
    (1, 's_mov_b32 s2, s9'),
    (1, 's_mov_b32 s3, 0'),
    (1, 's_mov_b32 m0, s10'),
    (2, 's_load_dwordx8 s[0:7], s[2:3], 0'),
    (1, 'v_interp_p1_f32 v2, v0, attr0.x'),
    (1, 'v_interp_p1_f32 v3, v0, attr0.y'),
    (1, 'v_interp_p2_f32 v2, v1, attr0.x'),
    (1, 'v_interp_p2_f32 v3, v1, attr0.y'),
    (1, 's_and_b64 exec, exec, s[12:13]'),
    (1, 's_movk_i32 s8, 0x92'),
    (2, 's_mov_b32 s9, 0x00c00000'),
    (1, 's_brev_b32 s10, 40'),
    (1, 's_mov_b32 s11, 0'),
    (1, 's_waitcnt lgkmcnt(0)'),
    (2, 'image_sample v[0:3], v[2:3], s[0:7], s[8:11] dmask:0xf dim:2d'),
    (1, 's_waitcnt vmcnt(0)'),
    (1, 's_nop 0'),
    (1, 's_nop 0'),
    (1, 'v_cvt_pkrtz_f16_f32 v4, v0, v1'),
    (1, 'v_cvt_pkrtz_f16_f32 v5, v2, v3'),
    (2, 'exp mrt0, v4, v5, compr done vm, rgba'),
    (1, 's_endpgm'),
)

HLSL = '''// Verified PS_CopyPoint profile. The harness binds one 2D SRV and a point-clamp sampler.
Texture2D<float4> input_texture : register(t0);
SamplerState point_clamp : register(s0);

// Direct3D's legacy f32-to-f16 conversion rounds toward zero for the
// finite normalized texels in this proof. Expand back for the float RTV.
float half_rtz(float value)
{
    return f16tof32(f32tof16(value));
}

float4 main(float2 uv : TEXCOORD0) : SV_Target0
{
    float4 sampled = input_texture.Sample(point_clamp, uv);
    return float4(half_rtz(sampled.r), half_rtz(sampled.g),
                  half_rtz(sampled.b), half_rtz(sampled.a));
}
'''


def extract():
    with (HERE.parent.parent / 'shaders.rpf').open('rb') as archive:
        archive.seek(ARCHIVE_OFFSET)
        magic, size, output_size = struct.unpack('<III', archive.read(12))
        if (magic, size, output_size) != (0xDDCE7, 1190, 1528):
            raise ValueError('The selected archive wrapper differs from the verified profile')
        container = inflate(archive.read(size - 12), output_size)
    if hashlib.sha256(container).hexdigest() != CONTAINER_SHA256:
        raise ValueError('Shader container digest differs from the verified profile')
    record, payloads = decode(container)
    if record['program_kind'] != 0 or len(payloads) != 1:
        raise ValueError('Expected one native pixel shader')
    chunks = {}
    for part in dxbc_at(container, 0)['parts']:
        offset = part['offset'] + 8
        chunks[part['tag']] = container[offset:offset + part['size']]
    if len(chunks['HASH']) != 20:
        raise ValueError('Expected a 16-byte shader digest')
    return payloads[0][1], chunks


def signature(chunk):
    count, offset = struct.unpack_from('<II', chunk)
    if offset != 8 or offset + count * 32 > len(chunk):
        raise ValueError('Invalid signature table')
    elements = []
    for index in range(count):
        _, name, semantic, _, component, register, mask = struct.unpack_from(
            '<6IB', chunk, offset + index * 32
        )
        text = chunk[name:].split(b'\0', 1)[0].lower()
        elements.append((text, semantic, component, register, mask))
    return elements


def resource_bindings(chunk):
    runtime_size, = struct.unpack_from('<I', chunk)
    header_end = runtime_size + 4
    if header_end + 8 > len(chunk):
        raise ValueError('Truncated PSV0 resource header')
    count, stride = struct.unpack_from('<II', chunk, header_end)
    table_start = header_end + 8
    if count != 2 or stride != 24 or table_start + count * stride > len(chunk):
        raise ValueError('Expected two complete PSV0 resource bindings')
    return [struct.unpack_from('<6I', chunk, table_start + i * stride) for i in range(count)]


def disassemble(code, digest):
    if len(code) != 320:
        raise ValueError('Unexpected native code region size')
    if hashlib.sha256(code).hexdigest() != NATIVE_CODE_SHA256:
        raise ValueError('Native code digest differs from the verified profile')
    listing = []

    def emit(offset, words, instruction):
        encoding = ' '.join(f'{word:08x}' for word in words)
        listing.append(f'{offset:04x}  {encoding:26}  {instruction}')

    embedded_digest = bytearray()
    for offset in (0, 12):
        first, sources, literal = struct.unpack_from('<III', code, offset)
        if first >> 16 != 0xD580 or (sources >> 18) & 0x1FF != 0xFF:
            raise ValueError('Unexpected shader digest prologue')
        embedded_digest.extend(struct.pack('<HBBI', first & 65535, sources & 255,
                                           (sources >> 9) & 255, literal))
        emit(offset, (first, sources, literal), 'v_nop (Xbox shader digest)')
    if embedded_digest != digest:
        raise ValueError('Native prologue digest differs from the HASH chunk')

    offset = 24
    for word_count, instruction in BODY:
        words = struct.unpack_from(f'<{word_count}I', code, offset)
        emit(offset, words, instruction)
        offset += word_count * 4
    padding_words = (len(code) - offset) // 4
    if padding_words < 5 or code[offset:] != CODE_END * padding_words:
        raise ValueError('Expected S_CODE_END padding after the program')
    listing.append(f'{offset:04x}  bf9f0000 x {padding_words:<14}  s_code_end padding')
    return listing, offset


def main():
    code, chunks = extract()
    if signature(chunks['ISG1']) != [(b'sv_position', 0, 3, 0, 15), (b'texcoord', 0, 3, 1, 3)]:
        raise ValueError('Expected float2 TEXCOORD0 and the unused SV_Position signature')
    if signature(chunks['OSG1']) != [(b'sv_target', 0, 3, 0, 15)]:
        raise ValueError('Expected a full float4 SV_Target0 output')
    if resource_bindings(chunks['PSV0']) != [(1, 1, 0, 0, 14, 0), (3, 1, 85, 85, 2, 0)]:
        raise ValueError('Expected Xbox sampler s0 space1 and Texture2D t85 space1')
    listing, length = disassemble(code, chunks['HASH'][4:])
    header = f'{NAME} at archive offset {ARCHIVE_OFFSET:#x}\n'
    header += f'Native code SHA-256 {hashlib.sha256(code).hexdigest()}\n\n'
    header += 'Bindings: Xbox t85 space1 -> desktop t0, Xbox s0 space1 -> desktop s0.\n\n'
    (HERE / 'copy_point.rdna2.txt').write_text(header + '\n'.join(listing) + '\n', encoding='utf-8')
    (HERE / 'copy_point.hlsl').write_text(HLSL, encoding='utf-8')
    print(f'Verified {len(listing) - 1} instructions in {length} bytes and emitted the desktop shader.')


if __name__ == '__main__':
    main()
