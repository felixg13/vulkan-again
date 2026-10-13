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
[[vk::binding(0, 1)]] Sampler2D base_color;

struct VSOutput {
    float4 Pos : SV_POSITION;
    float3 Normal;
    float2 UV;
    float3 LightDir; // view-space direction the light travels
    float3 ViewVec;
};

[shader("vertex")]
VSOutput main(VSInput input, uniform PerDrawData *per_draw) {
    VSOutput output;
    float4x4 modelMat = per_draw->transform;
    output.Normal = mul((float3x3)mul(per_frame.view, modelMat), input.Normal);
    output.UV = input.UV;
    output.Pos = mul(per_frame.projection, mul(per_frame.view, mul(modelMat, float4(input.Pos.xyz, 1.0))));
    // Calculate view vectors required for lighting
    float4 fragPos = mul(mul(per_frame.view, modelMat), float4(input.Pos.xyz, 1.0));
    output.LightDir = mul((float3x3)per_frame.view, per_frame.light_dir.xyz);
    output.ViewVec = -fragPos.xyz;
    return output;
}

[shader("fragment")]
float4 main(VSOutput input) {
    // Phong lighting
    float3 N = normalize(input.Normal);
    float3 L = normalize(-input.LightDir);
    float3 V = normalize(input.ViewVec);
    float3 R = reflect(-L, N);
    float3 diffuse = max(dot(N, L), 0.0025);
    float3 specular = pow(max(dot(R, V), 0.0), 16.0) * 0.75;
    // Sample from texture
    float3 color = base_color.Sample(input.UV).rgb;
    return float4(diffuse * color.rgb + specular, 1.0);
}
