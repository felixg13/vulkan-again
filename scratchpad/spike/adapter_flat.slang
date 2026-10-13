struct EngineVaryings {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 world_position;
    [[vk::location(1)]] float3 world_normal;
    [[vk::location(2)]] float2 uv;
};
[shader("fragment")]
float4 pixel_main(EngineVaryings v) : SV_Target {
    VertexData d;
    d.SV_Position = v.position;
    d.normalWorld = normalize(v.world_normal);
    d.tangentWorld = float3(1, 0, 0);
    d.positionWorld = v.world_position;
    return fragmentMain(d);
}
