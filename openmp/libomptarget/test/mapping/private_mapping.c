// RUN: %libomptarget-compile-run-and-check-aarch64-unknown-linux-gnu
// RUN: %libomptarget-compile-run-and-check-powerpc64-ibm-linux-gnu
// RUN: %libomptarget-compile-run-and-check-powerpc64le-ibm-linux-gnu
// RUN: %libomptarget-compile-run-and-check-x86_64-pc-linux-gnu
// RUN: %libomptarget-compile-run-and-check-nvptx64-nvidia-cuda
// RUN: %libomptarget-compile-run-and-check-amdgcn-amd-amdhsa

#include <assert.h>
#include <stdio.h>

int main() {
  int data1[3] = {1}, data2[3] = {2}, data3[3] = {3};
  int sum[16] = {0};
#pragma omp target teams distribute parallel for map(tofrom                    \
                                                     : sum)                    \
    firstprivate(data1, data2, data3)
  for (int i = 0; i < 16; ++i) {
    for (int j = 0; j < 3; ++j) {
      sum[i] += data1[j];
      sum[i] += data2[j];
      sum[i] += data3[j];
    }
  }

  for (int i = 0; i < 16; ++i) {
    assert(sum[i] == 6);
  }

  printf("PASS\n");

  return 0;
}

// CHECK: PASS
