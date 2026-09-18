#version 430

layout(local_size_x = 256) in;

layout(std430, binding = 0) readonly buffer InputA {
  float a[];
};

layout(std430, binding = 1) readonly buffer InputB {
  float b[];
};

layout(std430, binding = 2) writeonly buffer OutputC {
  float c[];
};

uniform uint uCount;

void main() {
  uint i = gl_GlobalInvocationID.x;
  if (i < uCount)
    c[i] = a[i] + b[i];
}
