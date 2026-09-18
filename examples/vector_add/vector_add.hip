extern "C" __global__ void vector_add(const float *a, const float *b, float *c,
                                      unsigned int n) {
  const unsigned int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < n)
    c[i] = a[i] + b[i];
}
