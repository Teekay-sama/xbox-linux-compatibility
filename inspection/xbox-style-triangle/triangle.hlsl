struct VertexOutput {
    float3 color : COLOR0;
    float4 position : SV_Position;
};

VertexOutput vertex_main(float2 position : POSITION, float3 color : COLOR0) {
    VertexOutput output;
    output.position = float4(position, 0, 1);
    output.color = color;
    return output;
}

float4 pixel_main(float3 color : COLOR0) : SV_Target0 {
    return float4(color, 1);
}

struct TextureVertexOutput {
    float2 uv : TEXCOORD0;
    float4 position : SV_Position;
};

TextureVertexOutput texture_vertex_main(float2 position : POSITION, float2 uv : TEXCOORD0) {
    TextureVertexOutput output;
    output.position = float4(position, 0, 1);
    output.uv = uv;
    return output;
}
