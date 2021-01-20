// REQUIRES: amdgpu-registered-target
// RUN:   %clang -### -fopenmp -fopenmp-targets=amdgcn-amd-amdhsa -Xopenmp-target -march=gfx906 %s 2>&1 \
// RUN:   | FileCheck %s

// verify the tools invocations
// CHECK: clang{{.*}}"-cc1" "-triple" "x86_64-unknown-linux-gnu"{{.*}}"-x" "c"{{.*}}
// CHECK: clang{{.*}}"-cc1" "-triple" "x86_64-unknown-linux-gnu"{{.*}}"-x" "ir"{{.*}}
// CHECK: clang{{.*}}"-cc1"{{.*}}"-triple" "amdgcn-amd-amdhsa" "-aux-triple" "x86_64-unknown-linux-gnu" "-emit-llvm-bc" "-emit-llvm-uselists"{{.*}}"-target-cpu" "gfx906" "-fcuda-is-device"{{.*}}"-fopenmp" "-fopenmp-cuda-parallel-target-regions"{{.*}}"-fopenmp-is-device"{{.*}}"-o" {{.*}}amdgpu-openmp-toolchain-{{.*}}.bc{{.*}}"-x" "c"{{.*}}amdgpu-openmp-toolchain.c{{.*}}
// CHECK: llvm-link{{.*}}amdgpu-openmp-toolchain-{{.*}}.bc" "-o" "{{.*}}amdgpu-openmp-toolchain-{{.*}}-gfx906-linked-{{.*}}.bc"
// CHECK: opt{{.*}}amdgpu-openmp-toolchain-{{.*}}-gfx906-linked-{{.*}} "-mtriple=amdgcn-amd-amdhsa" "-mcpu=gfx906" "-o"{{.*}}amdgpu-openmp-toolchain-{{.*}}-gfx906-optimized-{{.*}}.bc"
// CHECK: llc{{.*}}amdgpu-openmp-toolchain-{{.*}}-gfx906-optimized-{{.*}}.bc" "-mtriple=amdgcn-amd-amdhsa" "-mcpu=gfx906" "-filetype=obj" "-o"{{.*}}amdgpu-openmp-toolchain-{{.*}}-gfx906-{{.*}}.o"
// CHECK: lld{{.*}}"-flavor" "gnu" "--no-undefined" "-shared" "-o"{{.*}}amdgpu-openmp-toolchain-{{.*}}.out" "{{.*}}amdgpu-openmp-toolchain-{{.*}}-gfx906-{{.*}}.o"
// CHECK: clang-offload-wrapper{{.*}}"-target" "x86_64-unknown-linux-gnu" "-o" "{{.*}}a-{{.*}}.bc" {{.*}}amdgpu-openmp-toolchain-{{.*}}.out"
// CHECK: clang{{.*}}"-cc1" "-triple" "x86_64-unknown-linux-gnu"{{.*}}"-o" "{{.*}}a-{{.*}}.o" "-x" "ir" "{{.*}}a-{{.*}}.bc"
// CHECK: ld{{.*}}"-o" "a.out"{{.*}}"{{.*}}amdgpu-openmp-toolchain-{{.*}}.o" "{{.*}}a-{{.*}}.o" "-lomp" "-lomptarget"

// RUN:   %clang -ccc-print-phases -fopenmp -fopenmp-targets=amdgcn-amd-amdhsa -Xopenmp-target -march=gfx906 %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-PHASES %s
// phases
// CHECK-PHASES: 0: input, "{{.*}}amdgpu-openmp-toolchain.c", c, (host-openmp)
// CHECK-PHASES: 1: preprocessor, {0}, cpp-output, (host-openmp)
// CHECK-PHASES: 2: compiler, {1}, ir, (host-openmp)
// CHECK-PHASES: 3: backend, {2}, assembler, (host-openmp)
// CHECK-PHASES: 4: assembler, {3}, object, (host-openmp)
// CHECK-PHASES: 5: input, "{{.*}}amdgpu-openmp-toolchain.c", c, (device-openmp)
// CHECK-PHASES: 6: preprocessor, {5}, cpp-output, (device-openmp)
// CHECK-PHASES: 7: compiler, {6}, ir, (device-openmp)
// CHECK-PHASES: 8: offload, "host-openmp (x86_64-unknown-linux-gnu)" {2}, "device-openmp (amdgcn-amd-amdhsa)" {7}, ir
// CHECK-PHASES: 9: linker, {8}, image, (device-openmp)
// CHECK-PHASES: 10: offload, "device-openmp (amdgcn-amd-amdhsa)" {9}, image
// CHECK-PHASES: 11: clang-offload-wrapper, {10}, ir, (host-openmp)
// CHECK-PHASES: 12: backend, {11}, assembler, (host-openmp)
// CHECK-PHASES: 13: assembler, {12}, object, (host-openmp)
// CHECK-PHASES: 14: linker, {4, 13}, image, (host-openmp)

