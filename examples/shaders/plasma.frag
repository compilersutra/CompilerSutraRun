#version 450

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

void main() {
  // Cheap plasma-style pattern in UV space
  float x = vUv.x * 6.28318;
  float y = vUv.y * 6.28318;
  float v = sin(x) + sin(y * 1.3) + sin((x + y) * 0.7) + sin(length(vUv - 0.5) * 12.0);
  v = v * 0.25 + 0.5;
  vec3 c = vec3(
      0.5 + 0.5 * sin(v * 6.28318),
      0.5 + 0.5 * sin(v * 6.28318 + 2.094),
      0.5 + 0.5 * sin(v * 6.28318 + 4.188));
  outColor = vec4(c, 1.0);
}
