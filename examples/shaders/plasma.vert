#version 450

// Fullscreen triangle (no VBO): covers NDC with gl_VertexIndex 0..2
vec2 positions[3] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 3.0, -1.0),
    vec2(-1.0,  3.0)
);

layout(location = 0) out vec2 vUv;

void main() {
  vec2 p = positions[gl_VertexIndex];
  gl_Position = vec4(p, 0.0, 1.0);
  vUv = p * 0.5 + 0.5;
}
