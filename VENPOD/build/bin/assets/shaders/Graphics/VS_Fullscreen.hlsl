// =============================================================================
// VENPOD Fullscreen Vertex Shader
// Generates a fullscreen triangle using vertex ID (no vertex buffer needed)
// =============================================================================

struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput main(uint vertexID : SV_VertexID) {
    VSOutput output;

    // Generate fullscreen triangle from vertex ID
    // Vertex 0: (-1, -1) -> UV (0, 1)
    // Vertex 1: ( 3, -1) -> UV (2, 1)
    // Vertex 2: (-1,  3) -> UV (0, -1)
    // This creates a triangle that covers the entire screen

    float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
    output.position = float4(uv * 2.0f - 1.0f, 0.0f, 1.0f);
    output.uv = float2(uv.x, 1.0f - uv.y);  // Flip Y for DX convention

    return output;
}
