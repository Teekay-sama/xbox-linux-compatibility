struct VertexOutput {
    float2 uv : TEXCOORD0;
    float4 position : SV_Position;
};

VertexOutput main(uint id : SV_VertexID) {
    const float2 position[3] = { float2(-1, -1), float2(-1, 3), float2(3, -1) };
    const float2 uv[3] = { float2(0, 1), float2(0, -1), float2(2, 1) };
    VertexOutput output;
    output.position = float4(position[id], 0, 1);
    output.uv = uv[id];
    return output;
}
