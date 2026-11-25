// Verified PS_CopyPoint profile. The harness binds one 2D SRV and a point-clamp sampler.
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
