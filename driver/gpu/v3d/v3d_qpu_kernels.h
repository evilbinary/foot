/* QPU 2D 内核 blob（来自 EwokOS libvideocore/src/g2d_qpu_kernels.h）。
 * 每条 64 位 = V3D 7.1 的两条 32 位 QPU 指令。
 * 只保留真机验证过的 argb_fill（纯写路径）。
 * argb_blit / argb_alpha / argb_copy 已删除：它们的 TMU 读路径依赖
 * EwokOS 未跑通的 vec4 模式，在本项目的 CSD 提交方式下恒返回 0，
 * 我们自己生成的 copy_vec / alpha_vec 才是实际在用的（见文件末尾）。 */
#ifndef V3D_QPU_KERNELS_H
#define V3D_QPU_KERNELS_H
#include <stdint.h>


/* kernel argb_fill: 96 instructions */
const uint64_t g2d_qpu_argb_fill[] = {
    0x39800000bb03f000ULL,  /*   nop */
    0x39804000bb03f000ULL,  /*   nop */
    0x39808000bb03f000ULL,  /*   nop */
    0x3980c000bb03f000ULL,  /*   nop */
    0x39810000bb03f000ULL,  /*   nop */
    0x39814000bb03f000ULL,  /*   nop */
    0x39818000bb03f000ULL,  /*   nop */
    0x3981c000bb03f000ULL,  /*   nop */
    0x39820000bb03f000ULL,  /*   nop */
    0x39824000bb03f000ULL,  /*   nop */
    0x39828000bb03f000ULL,  /*   nop */
    0x39850000bb03f000ULL,  /*   nop */
    0x39854000bb03f000ULL,  /*   nop */
    0x39858000bb03f000ULL,  /*   nop */
    0x3985c000bb03f000ULL,  /*   nop */
    0x39860000bb03f000ULL,  /*   nop */
    0x3800000bbb03f002ULL,  /*   eidx */
    0x3800000cf903f2c3ULL,  /*   mov */
    0x39e000117c03f2c2ULL,  /*   shl */
    0x380000103803f191ULL,  /*   add */
    0x38000011bb03f001ULL,  /*   tidx */
    0x39e000117d03f442ULL,  /*   shr */
    0x39e00011b503f44fULL,  /*   and */
    0x39e000183803f441ULL,  /*   add */
    0x39e040183c03f601ULL,  /*   sub */
    0x02ffffddff00d000ULL,  /*   branch ANYNA */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x24000440bb455000ULL,  /*   nop */
    0x3800000ef903f443ULL,  /*   mov */
    0x39e040183c03f501ULL,  /*   sub */
    0x380a000e3803f389ULL,  /*   add */
    0x24000440bb442000ULL,  /*   nop */
    0x380000103803f411ULL,  /*   add */
    0x08048440bb0ce000ULL,  /*   nop */
    0x020001160000d000ULL,  /*   branch ANYA */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x380000183c03f0ceULL,  /*   sub */
    0x39e000183803f601ULL,  /*   add */
    0x380000157803f558ULL,  /*   min */
    0x39e000183803f041ULL,  /*   add */
    0x24000140bb615000ULL,  /*   nop */
    0x3800000ff903f303ULL,  /*   mov */
    0x3800000df903f043ULL,  /*   mov */
    0x39c00012f903f3c3ULL,  /*   mov */
    0x39e000123803f481ULL,  /*   add */
    0x39e000137c03f482ULL,  /*   shl */
    0x39e040113c03f501ULL,  /*   sub */
    0x39ea000cb503f1d0ULL,  /*   and */
    0x380a000c3803f30bULL,  /*   add */
    0x380a000ff903f303ULL,  /*   mov */
    0x020000d60000d000ULL,  /*   branch ANYA */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x02fffffdff00d000ULL,  /*   branch ANYNA */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x3800001af903f403ULL,  /*   mov */
    0x39c04011f903f003ULL,  /*   mov */
    0x380240113c03f3c7ULL,  /*   sub */
    0x380200113c03f3c8ULL,  /*   sub */
    0x380240113c03f389ULL,  /*   sub */
    0x380200113c03f38aULL,  /*   sub */
    0x380a001af903f5c3ULL,  /*   mov */
    0x39e0a2cd3c003341ULL,  /*   sub */
    0x07f0038df9381043ULL,  /*   mov */
    0x0400040f384133d2ULL,  /*   add */
    0x0810040ff9404303ULL,  /*   mov */
    0x39e040053c03f141ULL,  /*   sub */
    0x02ffff85ff00d000ULL,  /*   branch ANYNA */
    0x3800100cf903f683ULL,  /*   mov */
    0x38000000bb03f000ULL,  /*   nop */
    0x38001006bb03f00fULL,  /*   tmuwt */
    0x38200000bb03f000ULL,  /*   nop */
    0x38200000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38200000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x39e062c53c003141ULL,  /*   sub */
    0x02ffffddff00d000ULL,  /*   branch ANYNA */
    0x3800100cf903f403ULL,  /*   mov */
    0x380000103803f413ULL,  /*   add */
    0x38001006bb03f00fULL,  /*   tmuwt */
    0x38200000bb03f000ULL,  /*   nop */
    0x38200000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38200000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
};
const unsigned g2d_qpu_argb_fill_n = 96;

/* argb_copy：专门的 1:1 拷贝内核（不含仿射采样）*/
/* ============================================================
 * 自产 2D 内核（py-videocore7 汇编器生成，真机 65536/65536 验证）
 *
 * 生成方式：tools/qpudis/vc7_gen.py（勿手改本段）
 *   python3 tools/qpudis/vc7_gen.py > /tmp/k.h  然后并入本文件
 *
 * 与上面 EwokOS 原始 blob 的区别：
 *   - 全部由汇编器产出，指令编码与硬件一致（nop 的 0x3186 等）
 *   - 用 rf12/rf25 之外的普通寄存器做循环指针，避开 magic 段
 *   - TMU 走默认字节地址模式，不写 tmuc
 * ============================================================ */
static const uint64_t g2d_qpu_copy_vec[] = {
    0x398bf186bb03f000ULL,
    0x398c3186bb03f000ULL,
    0x398c7186bb03f000ULL,
    0x3986b186bb03f000ULL,
    0x39867186bb03f000ULL,
    0x38003186bb03f000ULL,
    0x38003186bb03f000ULL,
    0x38003186bb03f000ULL,
    0x38003186bb03f000ULL,
    0x38002198bb03f001ULL,
    0x39e021987d03f602ULL,
    0x0c001606bb619000ULL,
    0x380021af3803fbd8ULL,
    0x380021b03803fc18ULL,
    0x38002194bb03f002ULL,
    0x39e021957c03f502ULL,
    0x380021963803fbd5ULL,
    0x3800318cf903f583ULL,
    0x38003186bb03f000ULL,
    0x38003186bb03f000ULL,
    0x38873186bb03f000ULL,
    0x380021973803fc15ULL,
    0x3800318bf903f703ULL,
    0x3800318cf903f5c3ULL,
    0x38003186bb03f00fULL,
    0x380021af3803fbdaULL,
    0x380021b03803fc1aULL,
    0x39e061b13c03fc41ULL,
    0x02ffff73ff00d000ULL,
    0x38003186bb03f000ULL,
    0x38003186bb03f000ULL,
    0x38003186bb03f000ULL,
    0x38003186bb03f000ULL,
    0x38203186bb03f000ULL,
    0x38003186bb03f000ULL,
    0x38003186bb03f000ULL,
};
static const unsigned g2d_qpu_copy_vec_n = 36;

static const uint64_t g2d_qpu_alpha_vec[] = {
    0x398bf186bb03f000ULL,
    0x398c3186bb03f000ULL,
    0x398c7186bb03f000ULL,
    0x3986b186bb03f000ULL,
    0x398b7186bb03f000ULL,
    0x398b3186bb03f000ULL,
    0x398af186bb03f000ULL,
    0x398ab186bb03f000ULL,
    0x398a7186bb03f000ULL,
    0x398a3186bb03f000ULL,
    0x3988f186bb03f000ULL,
    0x38003186bb03f000ULL,
    0x38003186bb03f000ULL,
    0x38003186bb03f000ULL,
    0x380021a2bb03f001ULL,
    0x39e021a27d03f882ULL,
    0x0c001886bb8a3000ULL,
    0x380021af3803fbe2ULL,
    0x380021b03803fc22ULL,
    0x38002194bb03f002ULL,
    0x39e021957c03f502ULL,
    0x380021963803fbd5ULL,
    0x3800318cf903f583ULL,
    0x38003186bb03f000ULL,
    0x38003186bb03f000ULL,
    0x38873186bb03f000ULL,
    0x3800219b3803fc15ULL,
    0x3800318cf903f6c3ULL,
    0x38003186bb03f000ULL,
    0x38003186bb03f000ULL,
    0x38877186bb03f000ULL,
    0x38002190f903f743ULL,
    0x38002190b503f42bULL,
    0x0c001446bb42c000ULL,
    0x38002192b503f72bULL,
    0x0c001486bb4ad000ULL,
    0x380021913803f452ULL,
    0x380021917d03f46aULL,
    0x38002190f903f443ULL,
    0x380021937d03f76aULL,
    0x38002193b503f4ebULL,
    0x0c001446bb4ec000ULL,
    0x380021927d03f72aULL,
    0x38002192b503f4abULL,
    0x0c001486bb4ad000ULL,
    0x380021913803f452ULL,
    0x380021917d03f46aULL,
    0x380021917c03f46aULL,
    0x38002190b603f411ULL,
    0x380021937d03f769ULL,
    0x38002193b503f4ebULL,
    0x0c001446bb4ec000ULL,
    0x380021927d03f729ULL,
    0x38002192b503f4abULL,
    0x0c001486bb4ad000ULL,
    0x380021913803f452ULL,
    0x380021917d03f46aULL,
    0x380021917c03f469ULL,
    0x38002190b603f411ULL,
    0x380021937d03f768ULL,
    0x0c001446bb4ec000ULL,
    0x380021927d03f728ULL,
    0x0c001486bb4ad000ULL,
    0x380021913803f452ULL,
    0x380021917d03f46aULL,
    0x380021917c03f468ULL,
    0x38002190b603f411ULL,
    0x3800318bf903f403ULL,
    0x3800318cf903f6c3ULL,
    0x38003186bb03f00fULL,
    0x380021af3803fbdaULL,
    0x380021b03803fc1aULL,
    0x39e061b13c03fc41ULL,
    0x02fffe33ff00d000ULL,
    0x38003186bb03f000ULL,
    0x38003186bb03f000ULL,
    0x38003186bb03f000ULL,
    0x38003186bb03f000ULL,
    0x38203186bb03f000ULL,
    0x38003186bb03f000ULL,
    0x38003186bb03f000ULL,
};
static const unsigned g2d_qpu_alpha_vec_n = 81;

#endif /* V3D_QPU_KERNELS_H */
