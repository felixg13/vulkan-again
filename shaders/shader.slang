// Engine-owned vertex stage. The renderer concatenates this file after the
// MaterialX-generated pixel source and the per-material adapter (which defines
// `pixel_main` and consumes EngineVaryings), so everything here shares one
// module with generated code: keep the names below distinct from MaterialX's.
struct VSInput {
    float3 Pos;
    float3 Normal;
    float2 UV;
};

struct PerFrameData {
    float4x4 projection;
    float4x4 view;
    // World-space direction the light travels (unit vector, w = 0).
    float4 light_dir;
};

struct PerDrawData {
    float4x4 transform;
};

[[vk::binding(0, 0)]] ConstantBuffer<PerFrameData> per_frame;

// World-space position and normal: MaterialX's generated code shades in world
// space (normalWorld / positionWorld / u_viewPosition).
struct EngineVaryings {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 world_position;
    [[vk::location(1)]] float3 world_normal;
    [[vk::location(2)]] float2 uv;
};

[shader("vertex")]
EngineVaryings main(VSInput input, uniform PerDrawData *per_draw) {
    EngineVaryings output;
    float4x4 model = per_draw->transform;
    float4 world = mul(model, float4(input.Pos, 1.0));
    output.world_position = world.xyz;
    output.world_normal = mul((float3x3)model, input.Normal);
    output.uv = input.UV;
    output.position = mul(per_frame.projection, mul(per_frame.view, world));
    return output;
}
