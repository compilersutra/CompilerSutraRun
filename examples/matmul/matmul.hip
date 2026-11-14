extern "C" __global__ void matmul(const float *a, const float *b, float *c, unsigned int M,
                                  unsigned int N, unsigned int K) {
  const unsigned int col = blockIdx.x * blockDim.x + threadIdx.x;
  const unsigned int row = blockIdx.y * blockDim.y + threadIdx.y;
  if (row >= M || col >= N)
    return;
  float acc = 0.0f;
  for (unsigned int k = 0; k < K; ++k)
    acc += a[row * K + k] * b[k * N + col];
  c[row * N + col] = acc;
}
