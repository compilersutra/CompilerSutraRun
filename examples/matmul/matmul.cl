__kernel void matmul(__global const float *a, __global const float *b, __global float *c,
                     const uint M, const uint N, const uint K) {
  const uint col = get_global_id(0);
  const uint row = get_global_id(1);
  if (row >= M || col >= N)
    return;
  float acc = 0.0f;
  for (uint k = 0; k < K; ++k)
    acc += a[row * K + k] * b[k * N + col];
  c[row * N + col] = acc;
}
