struct VS_INPUT
{
    float3 pos : POSITION;
    float2 tex : TEXCOORD;
};

struct PS_INPUT
{
    float4 pos : SV_Position;
    float2 tex : TEXCOORD;
};

// Vertex shader
PS_INPUT VS(VS_INPUT input)
{
    PS_INPUT output;
    output.pos = float4(input.pos, 1.0f);
    output.tex = input.tex;
    
    return output;
}

// Recurso do pixel shader
Texture2D myTexture : register(t0);
SamplerState mySampler : register(s0);

// Pixel shader
float4 PS(PS_INPUT input) : SV_Target
{
    return myTexture.Sample(mySampler, input.tex);
}
