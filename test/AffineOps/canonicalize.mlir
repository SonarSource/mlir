// RUN: mlir-opt %s -split-input-file -canonicalize | FileCheck %s

// Affine maps for test case: compose_affine_maps_1dto2d_no_symbols
// CHECK-DAG: [[MAP0:#map[0-9]+]] = (d0) -> (d0 - 1)
// CHECK-DAG: [[MAP1:#map[0-9]+]] = (d0) -> (d0 + 1)

// Affine maps for test case: compose_affine_maps_1dto2d_with_symbols
// CHECK-DAG: [[MAP4:#map[0-9]+]] = (d0)[s0] -> (d0 - s0)
// CHECK-DAG: [[MAP6:#map[0-9]+]] = (d0)[s0] -> (d0 * 2 - s0 + 1)
// CHECK-DAG: [[MAP7:#map[0-9]+]] = (d0)[s0, s1] -> (d0 * 2 + s0 - s1)

// Affine map for test case: compose_affine_maps_d2_tile
// CHECK-DAG: [[MAP8:#map[0-9]+]] = (d0, d1)[s0] -> ((d0 ceildiv s0) * s0 + d1 mod s0)

// Affine maps for test case: compose_affine_maps_dependent_loads
// CHECK-DAG: [[MAP9:#map[0-9]+]] = (d0)[s0] -> (d0 + s0)
// CHECK-DAG: [[MAP10:#map[0-9]+]] = (d0)[s0] -> (d0 * s0)
// CHECK-DAG: [[MAP11:#map[0-9]+]] = (d0)[s0, s1] -> ((d0 + s1) ceildiv s0)
// CHECK-DAG: [[MAP12:#map[0-9]+]] = (d0)[s0] -> ((d0 - s0) * s0)

// Affine maps for test case: compose_affine_maps_diamond_dependency
// CHECK-DAG: [[MAP13A:#map[0-9]+]] = (d0) -> ((d0 + 6) ceildiv 8)
// CHECK-DAG: [[MAP13B:#map[0-9]+]] = (d0) -> ((d0 * 4 - 4) floordiv 3)

// Affine maps for test case: partial_fold_map
// CHECK-DAG: [[MAP15:#map[0-9]+]] = ()[s0, s1] -> (s0 - s1)

// Affine maps for test cases: symbolic_composition_*
// CHECK-DAG: [[map_symbolic_composition_a:#map[0-9]+]] = ()[s0] -> (s0 * 512)
// CHECK-DAG: [[map_symbolic_composition_b:#map[0-9]+]] = ()[s0] -> (s0 * 4)
// CHECK-DAG: [[map_symbolic_composition_c:#map[0-9]+]] = ()[s0, s1] -> (s0 * 3 + s1)
// CHECK-DAG: [[map_symbolic_composition_d:#map[0-9]+]] = ()[s0, s1] -> (s1 * 3 + s0)

// Affine maps for test cases: map_mix_dims_and_symbols_*
// CHECK-DAG: [[map_mix_dims_and_symbols_b:#map[0-9]+]] = ()[s0, s1] -> (s1 + s0 * 42 + 6)
// CHECK-DAG: [[map_mix_dims_and_symbols_c:#map[0-9]+]] = ()[s0, s1] -> (s1 * 4 + s0 * 168 - 4)
// CHECK-DAG: [[map_mix_dims_and_symbols_d:#map[0-9]+]] = ()[s0, s1] -> ((s1 + s0 * 42 + 6) ceildiv 8)
// CHECK-DAG: [[map_mix_dims_and_symbols_e:#map[0-9]+]] = ()[s0, s1] -> ((s1 * 4 + s0 * 168 - 4) floordiv 3)

// Affine maps for test case: symbolic_semi_affine
// CHECK-DAG: [[symbolic_semi_affine:#map[0-9]+]] = (d0)[s0] -> (d0 floordiv (s0 + 1))

// CHECK-LABEL: func @compose_affine_maps_1dto2d_no_symbols() {
func @compose_affine_maps_1dto2d_no_symbols() {
  %0 = alloc() : memref<4x4xf32>

  affine.for %i0 = 0 to 15 {
    // Test load[%x, %x]

    %x0 = affine.apply (d0) -> (d0 - 1) (%i0)
    %x1_0 = affine.apply (d0, d1) -> (d0) (%x0, %x0)
    %x1_1 = affine.apply (d0, d1) -> (d1) (%x0, %x0)

    // CHECK: [[I0A:%[0-9]+]] = affine.apply [[MAP0]](%i0)
    // CHECK-NEXT: load %0{{\[}}[[I0A]], [[I0A]]{{\]}}
    %v0 = load %0[%x1_0, %x1_1] : memref<4x4xf32>

    // Test load[%y, %y]
    %y0 = affine.apply (d0) -> (d0 + 1) (%i0)
    %y1_0 = affine.apply (d0, d1) -> (d0) (%y0, %y0)
    %y1_1 = affine.apply (d0, d1) -> (d1) (%y0, %y0)

    // CHECK-NEXT: [[I1A:%[0-9]+]] = affine.apply [[MAP1]](%i0)
    // CHECK-NEXT: load %0{{\[}}[[I1A]], [[I1A]]{{\]}}
    %v1 = load %0[%y1_0, %y1_1] : memref<4x4xf32>

    // Test load[%x, %y]
    %xy_0 = affine.apply (d0, d1) -> (d0) (%x0, %y0)
    %xy_1 = affine.apply (d0, d1) -> (d1) (%x0, %y0)

    // CHECK-NEXT: load %0{{\[}}[[I0A]], [[I1A]]{{\]}}
    %v2 = load %0[%xy_0, %xy_1] : memref<4x4xf32>

    // Test load[%y, %x]
    %yx_0 = affine.apply (d0, d1) -> (d0) (%y0, %x0)
    %yx_1 = affine.apply (d0, d1) -> (d1) (%y0, %x0)
    // CHECK-NEXT: load %0{{\[}}[[I1A]], [[I0A]]{{\]}}
    %v3 = load %0[%yx_0, %yx_1] : memref<4x4xf32>
  }
  return
}

// CHECK-LABEL: func @compose_affine_maps_1dto2d_with_symbols() {
func @compose_affine_maps_1dto2d_with_symbols() {
  %0 = alloc() : memref<4x4xf32>

  affine.for %i0 = 0 to 15 {
    // Test load[%x0, %x0] with symbol %c4
    %c4 = constant 4 : index
    %x0 = affine.apply (d0)[s0] -> (d0 - s0) (%i0)[%c4]

    // CHECK: [[I0:%[0-9]+]] = affine.apply [[MAP4]](%i0)[%c4]
    // CHECK-NEXT: load %{{[0-9]+}}{{\[}}[[I0]], [[I0]]{{\]}}
    %v0 = load %0[%x0, %x0] : memref<4x4xf32>

    // Test load[%x0, %x1] with symbol %c4 captured by '%x0' map.
    %x1 = affine.apply (d0) -> (d0 + 1) (%i0)
    %y1 = affine.apply (d0, d1) -> (d0+d1) (%x0, %x1)
    // CHECK-NEXT: [[I1:%[0-9]+]] = affine.apply [[MAP6]](%i0)[%c4]
    // CHECK-NEXT: load %{{[0-9]+}}{{\[}}[[I1]], [[I1]]{{\]}}
    %v1 = load %0[%y1, %y1] : memref<4x4xf32>

    // Test load[%x1, %x0] with symbol %c4 captured by '%x0' map.
    %y2 = affine.apply (d0, d1) -> (d0 + d1) (%x1, %x0)
    // CHECK-NEXT: [[I2:%[0-9]+]] = affine.apply [[MAP6]](%i0)[%c4]
    // CHECK-NEXT: load %{{[0-9]+}}{{\[}}[[I2]], [[I2]]{{\]}}
    %v2 = load %0[%y2, %y2] : memref<4x4xf32>

    // Test load[%x2, %x0] with symbol %c4 from '%x0' and %c5 from '%x2'
    %c5 = constant 5 : index
    %x2 = affine.apply (d0)[s0] -> (d0 + s0) (%i0)[%c5]
    %y3 = affine.apply (d0, d1) -> (d0 + d1) (%x2, %x0)
    // CHECK: [[I3:%[0-9]+]] = affine.apply [[MAP7]](%i0)[%c5, %c4]
    // CHECK-NEXT: load %{{[0-9]+}}{{\[}}[[I3]], [[I3]]{{\]}}
    %v3 = load %0[%y3, %y3] : memref<4x4xf32>
  }
  return
}

// CHECK-LABEL: func @compose_affine_maps_2d_tile() {
func @compose_affine_maps_2d_tile() {
  %0 = alloc() : memref<16x32xf32>
  %1 = alloc() : memref<16x32xf32>

  %c4 = constant 4 : index
  %c8 = constant 8 : index

  affine.for %i0 = 0 to 3 {
    %x0 = affine.apply (d0)[s0] -> (d0 ceildiv s0) (%i0)[%c4]
    affine.for %i1 = 0 to 3 {
      %x1 = affine.apply (d0)[s0] -> (d0 ceildiv s0) (%i1)[%c8]
      affine.for %i2 = 0 to 3 {
        %x2 = affine.apply (d0)[s0] -> (d0 mod s0) (%i2)[%c4]
        affine.for %i3 = 0 to 3 {
          %x3 = affine.apply (d0)[s0] -> (d0 mod s0) (%i3)[%c8]

          %x40 = affine.apply (d0, d1, d2, d3)[s0, s1] ->
            ((d0 * s0) + d2) (%x0, %x1, %x2, %x3)[%c4, %c8]
          %x41 = affine.apply (d0, d1, d2, d3)[s0, s1] ->
            ((d1 * s1) + d3) (%x0, %x1, %x2, %x3)[%c4, %c8]
          // CHECK: [[I0:%[0-9]+]] = affine.apply [[MAP8]](%i0, %i2)[%c4]
          // CHECK: [[I1:%[0-9]+]] = affine.apply [[MAP8]](%i1, %i3)[%c8]
          // CHECK-NEXT: [[L0:%[0-9]+]] = load %{{[0-9]+}}{{\[}}[[I0]], [[I1]]{{\]}}
          %v0 = load %0[%x40, %x41] : memref<16x32xf32>

          // CHECK-NEXT: store [[L0]], %{{[0-9]+}}{{\[}}[[I0]], [[I1]]{{\]}}
          store %v0, %1[%x40, %x41] : memref<16x32xf32>
        }
      }
    }
  }
  return
}

// CHECK-LABEL: func @compose_affine_maps_dependent_loads() {
func @compose_affine_maps_dependent_loads() {
  %0 = alloc() : memref<16x32xf32>
  %1 = alloc() : memref<16x32xf32>

  affine.for %i0 = 0 to 3 {
    affine.for %i1 = 0 to 3 {
      affine.for %i2 = 0 to 3 {
        %c3 = constant 3 : index
        %c7 = constant 7 : index

        %x00 = affine.apply (d0, d1, d2)[s0, s1] -> (d0 + s0)
            (%i0, %i1, %i2)[%c3, %c7]
        %x01 = affine.apply (d0, d1, d2)[s0, s1] -> (d1 - s1)
            (%i0, %i1, %i2)[%c3, %c7]
        %x02 = affine.apply (d0, d1, d2)[s0, s1] -> (d2 * s0)
            (%i0, %i1, %i2)[%c3, %c7]

        // CHECK: [[I0:%[0-9]+]] = affine.apply [[MAP9]](%i0)[%c3]
        // CHECK: [[I1:%[0-9]+]] = affine.apply [[MAP4]](%i1)[%c7]
        // CHECK: [[I2:%[0-9]+]] = affine.apply [[MAP10]](%i2)[%c3]
        // CHECK-NEXT: load %{{[0-9]+}}{{\[}}[[I0]], [[I1]]{{\]}}
        %v0 = load %0[%x00, %x01] : memref<16x32xf32>

        // CHECK-NEXT: load %{{[0-9]+}}{{\[}}[[I0]], [[I2]]{{\]}}
        %v1 = load %0[%x00, %x02] : memref<16x32xf32>

        // Swizzle %i0, %i1
        // CHECK-NEXT: load %{{[0-9]+}}{{\[}}[[I1]], [[I0]]{{\]}}
        %v2 = load %0[%x01, %x00] : memref<16x32xf32>

        // Swizzle %x00, %x01 and %c3, %c7
        %x10 = affine.apply (d0, d1)[s0, s1] -> (d0 * s1)
           (%x01, %x00)[%c7, %c3]
        %x11 = affine.apply (d0, d1)[s0, s1] -> (d1 ceildiv s0)
           (%x01, %x00)[%c7, %c3]

        // CHECK-NEXT: [[I2A:%[0-9]+]] = affine.apply [[MAP12]](%i1)[%c7]
        // CHECK-NEXT: [[I2B:%[0-9]+]] = affine.apply [[MAP11]](%i0)[%c3, %c7]
        // CHECK-NEXT: load %{{[0-9]+}}{{\[}}[[I2A]], [[I2B]]{{\]}}
        %v3 = load %0[%x10, %x11] : memref<16x32xf32>
      }
    }
  }
  return
}

// CHECK-LABEL: func @compose_affine_maps_diamond_dependency() {
func @compose_affine_maps_diamond_dependency() {
  %0 = alloc() : memref<4x4xf32>

  affine.for %i0 = 0 to 15 {
    %a = affine.apply (d0) -> (d0 - 1) (%i0)
    %b = affine.apply (d0) -> (d0 + 7) (%a)
    %c = affine.apply (d0) -> (d0 * 4) (%a)
    %d0 = affine.apply (d0, d1) -> (d0 ceildiv 8) (%b, %c)
    %d1 = affine.apply (d0, d1) -> (d1 floordiv 3) (%b, %c)
    // CHECK: [[I0:%[0-9]+]] = affine.apply [[MAP13A]](%i0)
    // CHECK: [[I1:%[0-9]+]] = affine.apply [[MAP13B]](%i0)
    // CHECK-NEXT: load %{{[0-9]+}}{{\[}}[[I0]], [[I1]]{{\]}}
    %v = load %0[%d0, %d1] : memref<4x4xf32>
  }

  return
}

// CHECK-LABEL: func @arg_used_as_dim_and_symbol
func @arg_used_as_dim_and_symbol(%arg0: memref<100x100xf32>, %arg1: index) {
  %c9 = constant 9 : index
  %1 = alloc() : memref<100x100xf32, 1>
  %2 = alloc() : memref<1xi32>
  affine.for %i0 = 0 to 100 {
    affine.for %i1 = 0 to 100 {
      %3 = affine.apply (d0, d1)[s0, s1] -> (d1 + s0 + s1)
        (%i0, %i1)[%arg1, %c9]
      %4 = affine.apply (d0, d1, d3) -> (d3 - (d0 + d1))
        (%arg1, %c9, %3)
      // CHECK: load %{{[0-9]+}}{{\[}}%i1, %arg1{{\]}}
      %5 = load %1[%4, %arg1] : memref<100x100xf32, 1>
    }
  }
  return
}

// CHECK-LABEL: func @trivial_maps
func @trivial_maps() {
  // CHECK-NOT: affine.apply

  %0 = alloc() : memref<10xf32>
  %c0 = constant 0 : index
  %cst = constant 0.000000e+00 : f32
  affine.for %i1 = 0 to 10 {
    %1 = affine.apply ()[s0] -> (s0)()[%c0]
    store %cst, %0[%1] : memref<10xf32>
    %2 = load %0[%c0] : memref<10xf32>

    %3 = affine.apply ()[] -> (0)()[]
    store %cst, %0[%3] : memref<10xf32>
    %4 = load %0[%c0] : memref<10xf32>
  }
  return
}

// CHECK-LABEL: func @partial_fold_map
func @partial_fold_map(%arg0: memref<index>, %arg1: index, %arg2: index) {
  // TODO: Constant fold one index into affine.apply
  %c42 = constant 42 : index
  %2 = affine.apply (d0, d1) -> (d0 - d1) (%arg1, %c42)
  store %2, %arg0[] : memref<index>
  // CHECK: [[X:%[0-9]+]] = affine.apply [[MAP15]]()[%arg1, %c42]
  // CHECK-NEXT: store [[X]], %arg0

  return
}

// CHECK-LABEL: func @symbolic_composition_a(%arg0: index, %arg1: index) -> index {
func @symbolic_composition_a(%arg0: index, %arg1: index) -> index {
  %0 = affine.apply (d0) -> (d0 * 4)(%arg0)
  %1 = affine.apply ()[s0, s1] -> (8 * s0)()[%0, %arg0]
  %2 = affine.apply ()[s0, s1] -> (16 * s1)()[%arg1, %1]
  // CHECK: %{{.*}} = affine.apply [[map_symbolic_composition_a]]()[%arg0]
  return %2 : index
}

// CHECK-LABEL: func @symbolic_composition_b(%arg0: index, %arg1: index, %arg2: index, %arg3: index) -> index {
func @symbolic_composition_b(%arg0: index, %arg1: index, %arg2: index, %arg3: index) -> index {
  %0 = affine.apply (d0) -> (d0)(%arg0)
  %1 = affine.apply ()[s0, s1, s2, s3] -> (s0 + s1 + s2 + s3)()[%0, %0, %0, %0]
  // CHECK: %{{.*}} = affine.apply [[map_symbolic_composition_b]]()[%arg0]
  return %1 : index
}

// CHECK-LABEL: func @symbolic_composition_c(%arg0: index, %arg1: index, %arg2: index, %arg3: index) -> index {
func @symbolic_composition_c(%arg0: index, %arg1: index, %arg2: index, %arg3: index) -> index {
  %0 = affine.apply (d0) -> (d0)(%arg0)
  %1 = affine.apply (d0) -> (d0)(%arg1)
  %2 = affine.apply ()[s0, s1, s2, s3] -> (s0 + s1 + s2 + s3)()[%0, %0, %0, %1]
  // CHECK: %{{.*}} = affine.apply [[map_symbolic_composition_c]]()[%arg0, %arg1]
  return %2 : index
}

// CHECK-LABEL: func @symbolic_composition_d(%arg0: index, %arg1: index, %arg2: index, %arg3: index) -> index {
func @symbolic_composition_d(%arg0: index, %arg1: index, %arg2: index, %arg3: index) -> index {
  %0 = affine.apply (d0) -> (d0)(%arg0)
  %1 = affine.apply ()[s0] -> (s0)()[%arg1]
  %2 = affine.apply ()[s0, s1, s2, s3] -> (s0 + s1 + s2 + s3)()[%0, %0, %0, %1]
  // CHECK: %{{.*}} = affine.apply [[map_symbolic_composition_d]]()[%arg1, %arg0]
  return %2 : index
}


// CHECK-LABEL: func @mix_dims_and_symbols_b(%arg0: index, %arg1: index) -> index {
func @mix_dims_and_symbols_b(%arg0: index, %arg1: index) -> index {
  %a = affine.apply (d0)[s0] -> (d0 - 1 + 42 * s0) (%arg0)[%arg1]
  %b = affine.apply (d0) -> (d0 + 7) (%a)
  // CHECK: {{.*}} = affine.apply [[map_mix_dims_and_symbols_b]]()[%arg1, %arg0]

  return %b : index
}

// CHECK-LABEL: func @mix_dims_and_symbols_c(%arg0: index, %arg1: index) -> index {
func @mix_dims_and_symbols_c(%arg0: index, %arg1: index) -> index {
  %a = affine.apply (d0)[s0] -> (d0 - 1 + 42 * s0) (%arg0)[%arg1]
  %b = affine.apply (d0) -> (d0 + 7) (%a)
  %c = affine.apply (d0) -> (d0 * 4) (%a)
  // CHECK: {{.*}} = affine.apply [[map_mix_dims_and_symbols_c]]()[%arg1, %arg0]
  return %c : index
}

// CHECK-LABEL: func @mix_dims_and_symbols_d(%arg0: index, %arg1: index) -> index {
func @mix_dims_and_symbols_d(%arg0: index, %arg1: index) -> index {
  %a = affine.apply (d0)[s0] -> (d0 - 1 + 42 * s0) (%arg0)[%arg1]
  %b = affine.apply (d0) -> (d0 + 7) (%a)
  %c = affine.apply (d0) -> (d0 * 4) (%a)
  %d = affine.apply ()[s0] -> (s0 ceildiv 8) ()[%b]
  // CHECK: {{.*}} = affine.apply [[map_mix_dims_and_symbols_d]]()[%arg1, %arg0]
  return %d : index
}

// CHECK-LABEL: func @mix_dims_and_symbols_e(%arg0: index, %arg1: index) -> index {
func @mix_dims_and_symbols_e(%arg0: index, %arg1: index) -> index {
  %a = affine.apply (d0)[s0] -> (d0 - 1 + 42 * s0) (%arg0)[%arg1]
  %b = affine.apply (d0) -> (d0 + 7) (%a)
  %c = affine.apply (d0) -> (d0 * 4) (%a)
  %d = affine.apply ()[s0] -> (s0 ceildiv 8) ()[%b]
  %e = affine.apply (d0) -> (d0 floordiv 3) (%c)
  // CHECK: {{.*}} = affine.apply [[map_mix_dims_and_symbols_e]]()[%arg1, %arg0]
  return %e : index
}

// CHECK-LABEL: func @mix_dims_and_symbols_f(%arg0: index, %arg1: index) -> index {
func @mix_dims_and_symbols_f(%arg0: index, %arg1: index) -> index {
  %a = affine.apply (d0)[s0] -> (d0 - 1 + 42 * s0) (%arg0)[%arg1]
  %b = affine.apply (d0) -> (d0 + 7) (%a)
  %c = affine.apply (d0) -> (d0 * 4) (%a)
  %d = affine.apply ()[s0] -> (s0 ceildiv 8) ()[%b]
  %e = affine.apply (d0) -> (d0 floordiv 3) (%c)
  %f = affine.apply (d0, d1)[s0, s1] -> (d0 - s1 +  d1 - s0) (%d, %e)[%e, %d]
  // CHECK: {{.*}} = constant 0 : index

  return %f : index
}

// CHECK-LABEL: func @mix_dims_and_symbols_g(%arg0: index, %arg1: index) -> (index, index, index) {
func @mix_dims_and_symbols_g(%M: index, %N: index) -> (index, index, index) {
  %K = affine.apply (d0) -> (4*d0) (%M)
  %res1 = affine.apply ()[s0, s1] -> (4 * s0)()[%N, %K]
  %res2 = affine.apply ()[s0, s1] -> (s1)()[%N, %K]
  %res3 = affine.apply ()[s0, s1] -> (1024)()[%N, %K]
  // CHECK-DAG: {{.*}} = constant 1024 : index
  // CHECK-DAG: {{.*}} = affine.apply [[map_symbolic_composition_b]]()[%arg1]
  // CHECK-DAG: {{.*}} = affine.apply [[map_symbolic_composition_b]]()[%arg0]
  return %res1, %res2, %res3 : index, index, index
}

// CHECK-LABEL: func @symbolic_semi_affine(%arg0: index, %arg1: index, %arg2: memref<?xf32>) {
func @symbolic_semi_affine(%M: index, %N: index, %A: memref<?xf32>) {
  %f1 = constant 1.0 : f32
  affine.for %i0 = 1 to 100 {
    %1 = affine.apply ()[s0] -> (s0 + 1) ()[%M]
    %2 = affine.apply (d0)[s0] -> (d0 floordiv s0) (%i0)[%1]
    // CHECK-DAG: {{.*}} = affine.apply [[symbolic_semi_affine]](%i0)[%arg0]
    store %f1, %A[%2] : memref<?xf32>
  }
  return
}

// -----

// CHECK: [[MAP0:#map[0-9]+]] = ()[s0] -> (0, s0)
// CHECK: [[MAP1:#map[0-9]+]] = ()[s0] -> (100, s0)

// CHECK-LABEL:  func @constant_fold_bounds(%arg0: index) {
func @constant_fold_bounds(%N : index) {
  // CHECK:      %c3 = constant 3 : index
  // CHECK-NEXT: %0 = "foo"() : () -> index
  %c9 = constant 9 : index
  %c1 = constant 1 : index
  %c2 = constant 2 : index
  %c3 = affine.apply (d0, d1) -> (d0 + d1) (%c1, %c2)
  %l = "foo"() : () -> index

  // CHECK:  affine.for %i0 = 5 to 7 {
  affine.for %i = max (d0, d1) -> (0, d0 + d1)(%c2, %c3) to min (d0, d1) -> (d0 - 2, 32*d1) (%c9, %c1) {
    "foo"(%i, %c3) : (index, index) -> ()
  }

  // Bound takes a non-constant argument but can still be folded.
  // CHECK:  affine.for %i1 = 1 to 7 {
  affine.for %j = max (d0) -> (0, 1)(%N) to min (d0, d1) -> (7, 9)(%N, %l) {
    "foo"(%j, %c3) : (index, index) -> ()
  }

  // None of the bounds can be folded.
  // CHECK: affine.for %i2 = max [[MAP0]]()[%0] to min [[MAP1]]()[%arg0] {
  affine.for %k = max ()[s0] -> (0, s0) ()[%l] to min ()[s0] -> (100, s0)()[%N] {
    "foo"(%k, %c3) : (index, index) -> ()
  }
  return
}