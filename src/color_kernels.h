#pragma once
// Project-owned full-range BT.709, planar 8-bit YUV444 <-> BGRA kernels.
// PTX 6.4 / sm_75 uses the Turing-era ISA: R500 drivers can JIT this, and
// newer GPUs use the same forward-compatible PTX. No CUDA runtime/NVRTC.
// RGB equations: Y=.2126R+.7152G+.0722B, U=(B-Y)/1.8556+128,
// V=(R-Y)/1.5748+128. Clamp to [0,255], round positive halves upward.
inline constexpr char HiveColorPtx[] = R"ptx(
.version 6.4
.target sm_75
.address_size 64
.visible .entry hive_bgra_to_yuv(
    .param .u64 rgba, .param .u64 yuv, .param .u64 pitch,
    .param .u32 width, .param .u32 height)
{
    .reg .pred p<3>;
    .reg .b32 r<20>;
    .reg .b64 d<12>;
    .reg .f32 f<12>;
    ld.param.u64 d0, [rgba];
    ld.param.u64 d1, [yuv];
    ld.param.u64 d2, [pitch];
    ld.param.u32 r0, [width];
    ld.param.u32 r1, [height];
    mov.u32 r2, %ctaid.x;
    mov.u32 r3, %ntid.x;
    mov.u32 r4, %tid.x;
    mad.lo.u32 r2, r2, r3, r4;
    mov.u32 r3, %ctaid.y;
    mov.u32 r4, %ntid.y;
    mov.u32 r5, %tid.y;
    mad.lo.u32 r3, r3, r4, r5;
    setp.ge.u32 p0, r2, r0;
    setp.ge.u32 p1, r3, r1;
    or.pred p2, p0, p1;
    @p2 ret;
    mul.wide.u32 d3, r3, r0;
    cvt.u64.u32 d4, r2;
    add.u64 d3, d3, d4;
    shl.b64 d3, d3, 2;
    add.u64 d3, d0, d3;
    cvt.u64.u32 d5, r3;
    mul.lo.u64 d5, d5, d2;
    add.u64 d5, d5, d4;
    add.u64 d5, d1, d5;
    cvt.u64.u32 d6, r1;
    mul.lo.u64 d6, d6, d2;
    add.u64 d7, d5, d6;
    add.u64 d8, d7, d6;

    ld.global.u8 r6, [d3];
    ld.global.u8 r7, [d3+1];
    ld.global.u8 r8, [d3+2];
    cvt.rn.f32.u32 f0, r6;
    cvt.rn.f32.u32 f1, r7;
    cvt.rn.f32.u32 f2, r8;
    mul.rn.f32 f3, f2, 0f3e59b3d0;
    fma.rn.f32 f3, f1, 0f3f371759, f3;
    fma.rn.f32 f3, f0, 0f3d93dd98, f3;
    sub.rn.f32 f4, f0, f3;
    div.rn.f32 f4, f4, 0f3fed844d;
    add.rn.f32 f4, f4, 0f43000000;
    sub.rn.f32 f5, f2, f3;
    div.rn.f32 f5, f5, 0f3fc9930c;
    add.rn.f32 f5, f5, 0f43000000;

    max.f32 f10, f3, 0f00000000;
    min.f32 f10, f10, 0f437f0000;
    add.rn.f32 f10, f10, 0f3f000000;
    cvt.rzi.u32.f32 r10, f10;
    st.global.u8 [d5], r10;

    max.f32 f10, f4, 0f00000000;
    min.f32 f10, f10, 0f437f0000;
    add.rn.f32 f10, f10, 0f3f000000;
    cvt.rzi.u32.f32 r10, f10;
    st.global.u8 [d7], r10;

    max.f32 f10, f5, 0f00000000;
    min.f32 f10, f10, 0f437f0000;
    add.rn.f32 f10, f10, 0f3f000000;
    cvt.rzi.u32.f32 r10, f10;
    st.global.u8 [d8], r10;
    ret;
}

.visible .entry hive_yuv_to_bgra(
    .param .u64 rgba, .param .u64 yuv, .param .u64 pitch,
    .param .u32 width, .param .u32 height)
{
    .reg .pred p<3>;
    .reg .b32 r<20>;
    .reg .b64 d<12>;
    .reg .f32 f<12>;
    ld.param.u64 d0, [rgba];
    ld.param.u64 d1, [yuv];
    ld.param.u64 d2, [pitch];
    ld.param.u32 r0, [width];
    ld.param.u32 r1, [height];
    mov.u32 r2, %ctaid.x;
    mov.u32 r3, %ntid.x;
    mov.u32 r4, %tid.x;
    mad.lo.u32 r2, r2, r3, r4;
    mov.u32 r3, %ctaid.y;
    mov.u32 r4, %ntid.y;
    mov.u32 r5, %tid.y;
    mad.lo.u32 r3, r3, r4, r5;
    setp.ge.u32 p0, r2, r0;
    setp.ge.u32 p1, r3, r1;
    or.pred p2, p0, p1;
    @p2 ret;
    mul.wide.u32 d3, r3, r0;
    cvt.u64.u32 d4, r2;
    add.u64 d3, d3, d4;
    shl.b64 d3, d3, 2;
    add.u64 d3, d0, d3;
    cvt.u64.u32 d5, r3;
    mul.lo.u64 d5, d5, d2;
    add.u64 d5, d5, d4;
    add.u64 d5, d1, d5;
    cvt.u64.u32 d6, r1;
    mul.lo.u64 d6, d6, d2;
    add.u64 d7, d5, d6;
    add.u64 d8, d7, d6;

    ld.global.u8 r6, [d5];
    ld.global.u8 r7, [d7];
    ld.global.u8 r8, [d8];
    cvt.rn.f32.u32 f0, r6;
    cvt.rn.f32.u32 f1, r7;
    cvt.rn.f32.u32 f2, r8;
    sub.rn.f32 f1, f1, 0f43000000;
    sub.rn.f32 f2, f2, 0f43000000;
    fma.rn.f32 f3, f1, 0f3fed844d, f0;
    fma.rn.f32 f4, f1, 0fbe3fd1ef, f0;
    fma.rn.f32 f4, f2, 0fbeefadfc, f4;
    fma.rn.f32 f5, f2, 0f3fc9930c, f0;

    max.f32 f10, f3, 0f00000000;
    min.f32 f10, f10, 0f437f0000;
    add.rn.f32 f10, f10, 0f3f000000;
    cvt.rzi.u32.f32 r10, f10;
    st.global.u8 [d3], r10;

    max.f32 f10, f4, 0f00000000;
    min.f32 f10, f10, 0f437f0000;
    add.rn.f32 f10, f10, 0f3f000000;
    cvt.rzi.u32.f32 r10, f10;
    st.global.u8 [d3+1], r10;

    max.f32 f10, f5, 0f00000000;
    min.f32 f10, f10, 0f437f0000;
    add.rn.f32 f10, f10, 0f3f000000;
    cvt.rzi.u32.f32 r10, f10;
    st.global.u8 [d3+2], r10;

    mov.u32 r10, 255;
    st.global.u8 [d3+3], r10;
    ret;
}
)ptx";

