#version 430

layout(local_size_x = 16, local_size_y = 16) in;

layout(std430, binding = 0) readonly buffer MatrixA { float a[]; };
layout(std430, binding = 1) readonly buffer MatrixB { float b[]; };
layout(std430, binding = 2) writeonly buffer MatrixC { float c[]; };

uniform uint uM;
uniform uint uN;
uniform uint uK;

void main() {
  uint col = gl_GlobalInvocationID.x;
  uint row = gl_GlobalInvocationID.y;
  if (row >= uM || col >= uN)
    return;
  float acc = 0.0;
  for (uint k = 0u; k < uK; ++k)
    acc += a[row * uK + k] * b[k * uN + col];
  c[row * uN + col] = acc;
}
