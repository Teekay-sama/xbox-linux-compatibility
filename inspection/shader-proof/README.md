# Xbox pixel shader proof

`PS_clearRipple_57383fac_Wrapped` from the local `shaders.rpf` was decoded,
lowered to HLSL, compiled with the Windows SDK DXC, and rendered through
Direct3D 12 WARP. All 256 pixels matched the CPU reference exactly.

## 1. Native instructions

The translator extracts the archive record at byte offset `0x5047c04` and
validates its container hash and full `SV_Target0` output signature. It
recovers 68 instruction bytes and 47 words of trailing padding. Two VOP3
no-ops encode the complete shader digest. The disassembly lists all 12
instructions with their offsets and encodings. The native program reads
no interpolants, textures, samplers, or other resources.

## 2. Desktop DXIL

The lowerer follows the vector register writes, packed half conversions,
and compressed MRT0 export. It folds the constant result into HLSL.
DXC compiles the generated source as `ps_6_0`. The DXIL dump shows four
`storeOutput.f32` calls, one per channel. The lowerer rejects instruction
forms and source operands outside the subset used in this proof.

## 3. Output comparison

The CPU reference can be derived independently from the instruction listing.
The two packed registers contain `0x38003800` and `0x00003c00`. Interpreting
their half-floats in export order gives `(0.5, 0.5, 1.0, 0.0)`.

The harness draws a full-screen triangle to a 16 × 16 `RGBA32_FLOAT` target.
It copies the target back to the CPU and checks each pixel against the reference.
The expected value passed at every pixel. A second comparison with alpha set
to `1` failed at all 256 pixels and returned exit code `1`.

The run uses Microsoft's software rasterizer. It verifies this shader's
constant output for the tested pipeline. Hardware execution, game integration,
and support for other shaders remain outside the proof.

## Reproduce

Run from the package root. Use Python 3, the Visual Studio x64 C++ tools,
and Windows SDK DXC. Set `$dxc` to the path of your x64 `dxc.exe`.

```powershell
python -B inspection/shader-proof/translate_clear_ripple.py
& $dxc -T ps_6_0 -E main -Fo inspection/shader-proof/clear_ripple.dxil inspection/shader-proof/clear_ripple.hlsl
& ./inspection/shader-proof/build_pixel.ps1 -Dxc $dxc
& ./inspection/shader-proof/run_pixel.exe ./inspection/shader-proof/clear_ripple.dxil 0.5 0.5 1 0
& ./inspection/shader-proof/run_pixel.exe ./inspection/shader-proof/clear_ripple.dxil 0.5 0.5 1 1
```

The final command is the mismatch control and should return `1`.

Format references include the [AMD RDNA2 ISA manual](https://www.amd.com/content/dam/amd/en/documents/radeon-tech-docs/instruction-set-architectures/rdna2-shader-instruction-set-architecture.pdf)
and [XCL's Xbox shader format notes](https://github.com/ivall/xcl/blob/main/docs/specs/2026-11-20-xcl-design.md).
