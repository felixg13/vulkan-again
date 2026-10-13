struct VSInput { float3 pos : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
struct PerFrameData { float4x4 projection; float4x4 view; float4 light_dir; };
struct PerDrawData { float4x4 transform; };
[[vk::binding(0, 0)]] ConstantBuffer<PerFrameData> per_frame;
struct EngineVaryings {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 world_position;
    [[vk::location(1)]] float3 world_normal;
    [[vk::location(2)]] float2 uv;
};
struct PushConstants { PerDrawData *draw; };
[[vk::push_constant]] PushConstants pc;
[shader("vertex")]
EngineVaryings vertex_main(VSInput i) {
    EngineVaryings o;
    float4x4 m = pc.draw->transform;
    float4 w = mul(m, float4(i.pos, 1.0));
    o.world_position = w.xyz;
    o.world_normal = mul((float3x3)m, i.normal);
    o.uv = i.uv;
    o.position = mul(per_frame.projection, mul(per_frame.view, w));
    return o;
}
