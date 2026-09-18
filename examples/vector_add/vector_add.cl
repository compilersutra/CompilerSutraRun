__kernel void vector_add(__global const float *a, __global const float *b,
                         __global float *c, const uint n) {
  const uint i = get_global_id(0);
  if (i < n)
    c[i] = a[i] + b[i];
}
