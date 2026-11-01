/* QPU 2D 内核 blob（来自 EwokOS libvideocore/src/g2d_qpu_kernels.h）。
 * 每条 64 位 = V3D 7.1 的两条 32 位 QPU 指令。
 * 目前只搬 2D 合成需要的三个：fill / blit / alpha。 */
#ifndef V3D_QPU_KERNELS_H
#define V3D_QPU_KERNELS_H
#include <stdint.h>

/* kernel argb_alpha: 344 instructions */
const uint64_t g2d_qpu_argb_alpha[] = {
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
    0x3982c000bb03f000ULL,  /*   nop */
    0x39830000bb03f000ULL,  /*   nop */
    0x39834000bb03f000ULL,  /*   nop */
    0x39838000bb03f000ULL,  /*   nop */
    0x3983c000bb03f000ULL,  /*   nop */
    0x39840000bb03f000ULL,  /*   nop */
    0x39844000bb03f000ULL,  /*   nop */
    0x39848000bb03f000ULL,  /*   nop */
    0x3984c000bb03f000ULL,  /*   nop */
    0x39850000bb03f000ULL,  /*   nop */
    0x3987c000bb03f000ULL,  /*   nop */
    0x39878000bb03f000ULL,  /*   nop */
    0x39874000bb03f000ULL,  /*   nop */
    0x39e000157d03f2c7ULL,  /*   shr */
    0x3800000b3803f2d5ULL,  /*   add */
    0x38000015bb03f002ULL,  /*   eidx */
    0x38000016f903f543ULL,  /*   mov */
    0x39e000157c03f542ULL,  /*   shl */
    0x3800001a3803f1d5ULL,  /*   add */
    0x38000015bb03f001ULL,  /*   tidx */
    0x39e000157d03f542ULL,  /*   shr */
    0x39e00015b503f54fULL,  /*   and */
    0x24000700bb554000ULL,  /*   nop */
    0x38000018f903f703ULL,  /*   mov */
    0x380000183803f60fULL,  /*   add */
    0x24000700bb55f000ULL,  /*   nop */
    0x3800001a3803f69cULL,  /*   add */
    0x08048700bb298000ULL,  /*   nop */
    0x020005160000d000ULL,  /*   branch ANYA */
    0x39e0001c7c03f546ULL,  /*   shl */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x3800001b3c03f298ULL,  /*   sub */
    0x39e0001b3803f6c1ULL,  /*   add */
    0x380000147803f51bULL,  /*   min */
    0x3800000a3803f79cULL,  /*   add */
    0x39e0001c7c03f582ULL,  /*   shl */
    0x3800000a3803f29cULL,  /*   add */
    0x39e0001c3803f241ULL,  /*   add */
    0x24000700bb714000ULL,  /*   nop */
    0x3800000cf903f703ULL,  /*   mov */
    0x38000019f903f583ULL,  /*   mov */
    0x38000017f903f243ULL,  /*   mov */
    0x39e0401c3c03f741ULL,  /*   sub */
    0x020004f60000d000ULL,  /*   branch ANYA */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x39e00019b503f350ULL,  /*   and */
    0x380000193803f656ULL,  /*   add */
    0x38000016f903f643ULL,  /*   mov */
    0x38200000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x24000540bb019000ULL,  /*   nop */
    0x25e006d57e09854fULL,  /*   asr */
    0x380000153803f541ULL,  /*   add */
    0x39e0001b7e03f6cfULL,  /*   asr */
    0x3800001b3803f6c3ULL,  /*   add */
    0x39e000157903f540ULL,  /*   max */
    0x380000157803f544ULL,  /*   min */
    0x39e0001b7903f6c0ULL,  /*   max */
    0x3800001b7803f6c5ULL,  /*   min */
    0x24000780bb6c6000ULL,  /*   nop */
    0x3800001e3803f795ULL,  /*   add */
    0x39e0001e7c03f782ULL,  /*   shl */
    0x3800001b3803f788ULL,  /*   add */
    0x3800100cf903f6c3ULL,  /*   mov */
    0x3800001ff903f283ULL,  /*   mov */
    0x39c04015f903f003ULL,  /*   mov */
    0x380240153c03f64dULL,  /*   sub */
    0x380200153c03f64eULL,  /*   sub */
    0x380240153c03f60fULL,  /*   sub */
    0x380200153c03f610ULL,  /*   sub */
    0x3808001ff903f683ULL,  /*   mov */
    0x3800100cf903f7c3ULL,  /*   mov */
    0x38870000bb03f000ULL,  /*   nop */
    0x38874000bb03f000ULL,  /*   nop */
    0x39e000157f03f718ULL,  /*   ror */
    0x38000015b503f553ULL,  /*   and */
    0x0c0006c0bb54b000ULL,  /*   nop */
    0x39e0001b7d03f6c8ULL,  /*   shr */
    0x380000143c03f4dbULL,  /*   sub */
    0x39e000157d03f6c7ULL,  /*   shr */
    0x3800001b3803f6d5ULL,  /*   add */
    0x39e000157d03f507ULL,  /*   shr */
    0x380000143803f515ULL,  /*   add */
    0x38000015b503f713ULL,  /*   and */
    0x0c000787b555b753ULL,  /*   and */
    0x0c0001c0bb1d4000ULL,  /*   nop */
    0x3800001e3803f787ULL,  /*   add */
    0x39e0001e7d03f788ULL,  /*   shr */
    0x39e000157f03f708ULL,  /*   ror */
    0x38000015b503f553ULL,  /*   and */
    0x0de007c77f55b748ULL,  /*   ror */
    0x38000007b503f1d3ULL,  /*   and */
    0x0c0001c0bb1d4000ULL,  /*   nop */
    0x3800001f3803f7c7ULL,  /*   add */
    0x39e0001f7d03f7c8ULL,  /*   shr */
    0x380000157d03f711ULL,  /*   shr */
    0x38000015b503f553ULL,  /*   and */
    0x0c0005477d55b751ULL,  /*   shr */
    0x38000007b503f1d3ULL,  /*   and */
    0x0c0001c0bb1d4000ULL,  /*   nop */
    0x380000153803f547ULL,  /*   add */
    0x39e000157d03f548ULL,  /*   shr */
    0x3800001ef703f79fULL,  /*   vpack */
    0x39e000077f03f758ULL,  /*   ror */
    0x38000007b503f1d3ULL,  /*   and */
    0x3800001f3c03f4c7ULL,  /*   sub */
    0x0c0007c0bb7db000ULL,  /*   nop */
    0x39e0001f7d03f7c8ULL,  /*   shr */
    0x380000073803f1dfULL,  /*   add */
    0x38000015f703f547ULL,  /*   vpack */
    0x3800001ef803f795ULL,  /*   v8pack */
    0x38000015f903f683ULL,  /*   mov */
    0x39c0401bf903f003ULL,  /*   mov */
    0x3802401b3c03f64dULL,  /*   sub */
    0x3802001b3c03f64eULL,  /*   sub */
    0x3802401b3c03f60fULL,  /*   sub */
    0x3802001b3c03f610ULL,  /*   sub */
    0x380a0015f903f283ULL,  /*   mov */
    0x380022c0bb783000ULL,  /*   nop */
    0x0be495ccf95c1543ULL,  /*   mov */
    0x07f00617f9601243ULL,  /*   mov */
    0x05e006557c651442ULL,  /*   shl */
    0x380c065a38583695ULL,  /*   add */
    0x3808001a3c03f692ULL,  /*   sub */
    0x24000540bb019000ULL,  /*   nop */
    0x25e006d57e09854fULL,  /*   asr */
    0x380000153803f541ULL,  /*   add */
    0x39e0001b7e03f6cfULL,  /*   asr */
    0x3800001b3803f6c3ULL,  /*   add */
    0x39e000157903f540ULL,  /*   max */
    0x380000157803f544ULL,  /*   min */
    0x39e0001b7903f6c0ULL,  /*   max */
    0x3800001b7803f6c5ULL,  /*   min */
    0x24000780bb6c6000ULL,  /*   nop */
    0x3800001e3803f795ULL,  /*   add */
    0x39e0001e7c03f782ULL,  /*   shl */
    0x3800001b3803f788ULL,  /*   add */
    0x3800100cf903f6c3ULL,  /*   mov */
    0x3800001ff903f283ULL,  /*   mov */
    0x39c04015f903f003ULL,  /*   mov */
    0x380240153c03f64dULL,  /*   sub */
    0x380200153c03f64eULL,  /*   sub */
    0x380240153c03f60fULL,  /*   sub */
    0x380200153c03f610ULL,  /*   sub */
    0x3808001ff903f683ULL,  /*   mov */
    0x3800100cf903f7c3ULL,  /*   mov */
    0x38870000bb03f000ULL,  /*   nop */
    0x38874000bb03f000ULL,  /*   nop */
    0x39e000157f03f718ULL,  /*   ror */
    0x38000015b503f553ULL,  /*   and */
    0x0c0006c0bb54b000ULL,  /*   nop */
    0x39e0001b7d03f6c8ULL,  /*   shr */
    0x380000143c03f4dbULL,  /*   sub */
    0x39e000157d03f6c7ULL,  /*   shr */
    0x3800001b3803f6d5ULL,  /*   add */
    0x39e000157d03f507ULL,  /*   shr */
    0x380000143803f515ULL,  /*   add */
    0x38000015b503f713ULL,  /*   and */
    0x0c000787b555b753ULL,  /*   and */
    0x0c0001c0bb1d4000ULL,  /*   nop */
    0x3800001e3803f787ULL,  /*   add */
    0x39e0001e7d03f788ULL,  /*   shr */
    0x39e000157f03f708ULL,  /*   ror */
    0x38000015b503f553ULL,  /*   and */
    0x0de007c77f55b748ULL,  /*   ror */
    0x38000007b503f1d3ULL,  /*   and */
    0x0c0001c0bb1d4000ULL,  /*   nop */
    0x3800001f3803f7c7ULL,  /*   add */
    0x39e0001f7d03f7c8ULL,  /*   shr */
    0x380000157d03f711ULL,  /*   shr */
    0x38000015b503f553ULL,  /*   and */
    0x0c0005477d55b751ULL,  /*   shr */
    0x38000007b503f1d3ULL,  /*   and */
    0x0c0001c0bb1d4000ULL,  /*   nop */
    0x380000153803f547ULL,  /*   add */
    0x39e000157d03f548ULL,  /*   shr */
    0x3800001ef703f79fULL,  /*   vpack */
    0x39e000077f03f758ULL,  /*   ror */
    0x38000007b503f1d3ULL,  /*   and */
    0x3800001f3c03f4c7ULL,  /*   sub */
    0x0c0007c0bb7db000ULL,  /*   nop */
    0x39e0001f7d03f7c8ULL,  /*   shr */
    0x380000073803f1dfULL,  /*   add */
    0x38000015f703f547ULL,  /*   vpack */
    0x3800001ef803f795ULL,  /*   v8pack */
    0x39e0400c3c03f301ULL,  /*   sub */
    0x02fffd8dff00d000ULL,  /*   branch ANYNA */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38001006bb03f00fULL,  /*   tmuwt */
    0x38201012bb03f00eULL,  /*   barrierid */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38200000bb03f000ULL,  /*   nop */
    0x38200000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38200000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38200000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x39e000127c03f442ULL,  /*   shl */
    0x24000640bb016000ULL,  /*   nop */
    0x24000600bb098000ULL,  /*   nop */
    0x39e0001b7e03f64fULL,  /*   asr */
    0x3800001b3803f6c1ULL,  /*   add */
    0x39e0001c7e03f60fULL,  /*   asr */
    0x3800001c3803f703ULL,  /*   add */
    0x24000700bb706000ULL,  /*   nop */
    0x3800001c3803f71bULL,  /*   add */
    0x39e0001c7c03f702ULL,  /*   shl */
    0x3800001b3803f708ULL,  /*   add */
    0x3800100cf903f6c3ULL,  /*   mov */
    0x3800100cf903f683ULL,  /*   mov */
    0x38870000bb03f000ULL,  /*   nop */
    0x38874000bb03f000ULL,  /*   nop */
    0x39e000157f03f718ULL,  /*   ror */
    0x38000015b503f553ULL,  /*   and */
    0x0c0006c0bb54b000ULL,  /*   nop */
    0x39e0001b7d03f6c8ULL,  /*   shr */
    0x380000143c03f4dbULL,  /*   sub */
    0x39e000157d03f6c7ULL,  /*   shr */
    0x3800001b3803f6d5ULL,  /*   add */
    0x39e000157d03f507ULL,  /*   shr */
    0x380000143803f515ULL,  /*   add */
    0x38000015b503f713ULL,  /*   and */
    0x0c000787b555b753ULL,  /*   and */
    0x0c0001c0bb1d4000ULL,  /*   nop */
    0x3800001e3803f787ULL,  /*   add */
    0x39e0001e7d03f788ULL,  /*   shr */
    0x39e000157f03f708ULL,  /*   ror */
    0x38000015b503f553ULL,  /*   and */
    0x0de007c77f55b748ULL,  /*   ror */
    0x38000007b503f1d3ULL,  /*   and */
    0x0c0001c0bb1d4000ULL,  /*   nop */
    0x3800001f3803f7c7ULL,  /*   add */
    0x39e0001f7d03f7c8ULL,  /*   shr */
    0x380000157d03f711ULL,  /*   shr */
    0x38000015b503f553ULL,  /*   and */
    0x0c0005477d55b751ULL,  /*   shr */
    0x38000007b503f1d3ULL,  /*   and */
    0x0c0001c0bb1d4000ULL,  /*   nop */
    0x380000153803f547ULL,  /*   add */
    0x39e000157d03f548ULL,  /*   shr */
    0x3800001ef703f79fULL,  /*   vpack */
    0x39e000077f03f758ULL,  /*   ror */
    0x38000007b503f1d3ULL,  /*   and */
    0x3800001f3c03f4c7ULL,  /*   sub */
    0x0c0007c0bb7db000ULL,  /*   nop */
    0x39e0001f7d03f7c8ULL,  /*   shr */
    0x380000073803f1dfULL,  /*   add */
    0x38000015f703f547ULL,  /*   vpack */
    0x3800001ef803f795ULL,  /*   v8pack */
    0x380022c0bb783000ULL,  /*   nop */
    0x0be495ccf95c1683ULL,  /*   mov */
    0x04100617f9602243ULL,  /*   mov */
    0x380000193803f64dULL,  /*   add */
    0x380800193c03f64eULL,  /*   sub */
    0x3800001a3803f692ULL,  /*   add */
    0x39e0001b7e03f64fULL,  /*   asr */
    0x3800001b3803f6c1ULL,  /*   add */
    0x39e0001c7e03f60fULL,  /*   asr */
    0x3800001c3803f703ULL,  /*   add */
    0x24000700bb706000ULL,  /*   nop */
    0x3800001c3803f71bULL,  /*   add */
    0x39e0001c7c03f702ULL,  /*   shl */
    0x3800001b3803f708ULL,  /*   add */
    0x3800100cf903f6c3ULL,  /*   mov */
    0x3800100cf903f683ULL,  /*   mov */
    0x38870000bb03f000ULL,  /*   nop */
    0x38874000bb03f000ULL,  /*   nop */
    0x39e000157f03f718ULL,  /*   ror */
    0x38000015b503f553ULL,  /*   and */
    0x0c0006c0bb54b000ULL,  /*   nop */
    0x39e0001b7d03f6c8ULL,  /*   shr */
    0x380000143c03f4dbULL,  /*   sub */
    0x39e000157d03f6c7ULL,  /*   shr */
    0x3800001b3803f6d5ULL,  /*   add */
    0x39e000157d03f507ULL,  /*   shr */
    0x380000143803f515ULL,  /*   add */
    0x38000015b503f713ULL,  /*   and */
    0x0c000787b555b753ULL,  /*   and */
    0x0c0001c0bb1d4000ULL,  /*   nop */
    0x3800001e3803f787ULL,  /*   add */
    0x39e0001e7d03f788ULL,  /*   shr */
    0x39e000157f03f708ULL,  /*   ror */
    0x38000015b503f553ULL,  /*   and */
    0x0de007c77f55b748ULL,  /*   ror */
    0x38000007b503f1d3ULL,  /*   and */
    0x0c0001c0bb1d4000ULL,  /*   nop */
    0x3800001f3803f7c7ULL,  /*   add */
    0x39e0001f7d03f7c8ULL,  /*   shr */
    0x380000157d03f711ULL,  /*   shr */
    0x38000015b503f553ULL,  /*   and */
    0x0c0005477d55b751ULL,  /*   shr */
    0x38000007b503f1d3ULL,  /*   and */
    0x0c0001c0bb1d4000ULL,  /*   nop */
    0x380000153803f547ULL,  /*   add */
    0x39e000157d03f548ULL,  /*   shr */
    0x3800001ef703f79fULL,  /*   vpack */
    0x39e000077f03f758ULL,  /*   ror */
    0x38000007b503f1d3ULL,  /*   and */
    0x3800001f3c03f4c7ULL,  /*   sub */
    0x0c0007c0bb7db000ULL,  /*   nop */
    0x39e0001f7d03f7c8ULL,  /*   shr */
    0x380000073803f1dfULL,  /*   add */
    0x38000015f703f547ULL,  /*   vpack */
    0x3800001ef803f795ULL,  /*   v8pack */
    0x39e0400c3c03f301ULL,  /*   sub */
    0x02fffe25ff00d000ULL,  /*   branch ANYNA */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38001006bb03f00fULL,  /*   tmuwt */
    0x38201012bb03f00eULL,  /*   barrierid */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38200000bb03f000ULL,  /*   nop */
    0x38200000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38200000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
};
const unsigned g2d_qpu_argb_alpha_n = 344;


/* kernel argb_blit: 220 instructions */
const uint64_t g2d_qpu_argb_blit[] = {
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
    0x3982c000bb03f000ULL,  /*   nop */
    0x39830000bb03f000ULL,  /*   nop */
    0x39834000bb03f000ULL,  /*   nop */
    0x39838000bb03f000ULL,  /*   nop */
    0x3983c000bb03f000ULL,  /*   nop */
    0x39840000bb03f000ULL,  /*   nop */
    0x39844000bb03f000ULL,  /*   nop */
    0x39848000bb03f000ULL,  /*   nop */
    0x3984c000bb03f000ULL,  /*   nop */
    0x39870000bb03f000ULL,  /*   nop */
    0x38000014bb03f002ULL,  /*   eidx */
    0x38000015f903f503ULL,  /*   mov */
    0x39e0001e7c03f502ULL,  /*   shl */
    0x380000193803f25eULL,  /*   add */
    0x3800001ff903f703ULL,  /*   mov */
    0x39830000bb03f000ULL,  /*   nop */
    0x39878000bb03f000ULL,  /*   nop */
    0x39874000bb03f000ULL,  /*   nop */
    0x39884000bb03f000ULL,  /*   nop */
    0x38000014bb03f001ULL,  /*   tidx */
    0x39e000147d03f502ULL,  /*   shr */
    0x39e00014b503f50fULL,  /*   and */
    0x24000700bb50c000ULL,  /*   nop */
    0x38000017f903f703ULL,  /*   mov */
    0x39e0401c3c03f7c1ULL,  /*   sub */
    0x380a00173803f5d2ULL,  /*   add */
    0x24000700bb51e000ULL,  /*   nop */
    0x380000193803f65cULL,  /*   add */
    0x08048700bb357000ULL,  /*   nop */
    0x020002560000d000ULL,  /*   branch ANYA */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x3800001c3c03f357ULL,  /*   sub */
    0x39e0001c3803f701ULL,  /*   add */
    0x3800000c7803f31cULL,  /*   min */
    0x39e0001c3803f2c1ULL,  /*   add */
    0x240003c0bb70c000ULL,  /*   nop */
    0x3800000df903f743ULL,  /*   mov */
    0x38000016f903f2c3ULL,  /*   mov */
    0x39c0001af903f3c3ULL,  /*   mov */
    0x39e0001a3803f681ULL,  /*   add */
    0x39e0001b7c03f682ULL,  /*   shl */
    0x38200000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x39e0401c3c03f7c1ULL,  /*   sub */
    0x0200022e0000d000ULL,  /*   branch ANYA */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x39e00018b503f410ULL,  /*   and */
    0x380000183803f615ULL,  /*   add */
    0x38000015f903f603ULL,  /*   mov */
    0x24000780bb018000ULL,  /*   nop */
    0x240007c0bb057000ULL,  /*   nop */
    0x3800001e3803f79fULL,  /*   add */
    0x39e0001e7e03f78fULL,  /*   asr */
    0x3800001e3803f784ULL,  /*   add */
    0x240007c0bb098000ULL,  /*   nop */
    0x24000700bb0d7000ULL,  /*   nop */
    0x3800001f3803f7dcULL,  /*   add */
    0x39e0001f7e03f7cfULL,  /*   asr */
    0x3800001f3803f7c5ULL,  /*   add */
    0x39e0001e7903f780ULL,  /*   max */
    0x3800001e7803f786ULL,  /*   min */
    0x39e0001f7903f7c0ULL,  /*   max */
    0x3800001f7803f7c7ULL,  /*   min */
    0x24000700bb7c8000ULL,  /*   nop */
    0x3800001c3803f71eULL,  /*   add */
    0x39e0001c7c03f702ULL,  /*   shl */
    0x3800001d3803f70aULL,  /*   add */
    0x3800100cf903f743ULL,  /*   mov */
    0x3887c000bb03f000ULL,  /*   nop */
    0x38000014f903f643ULL,  /*   mov */
    0x39c0401cf903f003ULL,  /*   mov */
    0x3802401c3c03f610ULL,  /*   sub */
    0x3802001c3c03f611ULL,  /*   sub */
    0x3802401c3c03f5d2ULL,  /*   sub */
    0x3802001c3c03f5d3ULL,  /*   sub */
    0x380a0014f903f343ULL,  /*   mov */
    0x3800100bf903f7c3ULL,  /*   mov */
    0x3800100cf903f503ULL,  /*   mov */
    0x0be485993858165bULL,  /*   add */
    0x07f005d6f95c12c3ULL,  /*   mov */
    0x380000183803f61aULL,  /*   add */
    0x08100658f964e543ULL,  /*   mov */
    0x24000700bb018000ULL,  /*   nop */
    0x24000780bb057000ULL,  /*   nop */
    0x2400079c3809871eULL,  /*   add */
    0x25e007dc7e0d770fULL,  /*   asr */
    0x3800001c3803f704ULL,  /*   add */
    0x3800001e3803f79fULL,  /*   add */
    0x39e0001e7e03f78fULL,  /*   asr */
    0x3800001e3803f785ULL,  /*   add */
    0x39e0001c7903f700ULL,  /*   max */
    0x3800001c7803f706ULL,  /*   min */
    0x39e0001e7903f780ULL,  /*   max */
    0x3800001e7803f787ULL,  /*   min */
    0x25e0474f3c7883c1ULL,  /*   sub */
    0x3800001d3803f75cULL,  /*   add */
    0x39e0001d7c03f742ULL,  /*   shl */
    0x3800001d3803f74aULL,  /*   add */
    0x02fffefdff00d000ULL,  /*   branch ANYNA */
    0x3800100cf903f743ULL,  /*   mov */
    0x3887c000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x3800100bf903f7c3ULL,  /*   mov */
    0x3800100cf903f343ULL,  /*   mov */
    0x38001006bb03f00fULL,  /*   tmuwt */
    0x38201012bb03f00eULL,  /*   barrierid */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38200000bb03f000ULL,  /*   nop */
    0x38200000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38200000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x24000780bb015000ULL,  /*   nop */
    0x240007c0bb057000ULL,  /*   nop */
    0x3800001e3803f79fULL,  /*   add */
    0x240007c0bb095000ULL,  /*   nop */
    0x24000700bb0d7000ULL,  /*   nop */
    0x3800001f3803f7dcULL,  /*   add */
    0x39e0001c7e03f78fULL,  /*   asr */
    0x3800001c3803f704ULL,  /*   add */
    0x39e0001d7e03f7cfULL,  /*   asr */
    0x3800001d3803f745ULL,  /*   add */
    0x39e0001c7903f700ULL,  /*   max */
    0x3800001c7803f706ULL,  /*   min */
    0x39e0001d7903f740ULL,  /*   max */
    0x3800001d7803f747ULL,  /*   min */
    0x24000740bb748000ULL,  /*   nop */
    0x3800001d3803f75cULL,  /*   add */
    0x39e0001d7c03f742ULL,  /*   shl */
    0x3800001d3803f74aULL,  /*   add */
    0x3800100cf903f743ULL,  /*   mov */
    0x38870000bb03f000ULL,  /*   nop */
    0x3800000ff903f303ULL,  /*   mov */
    0x39c0801d3c03f021ULL,  /*   sub */
    0x0200011e0000d000ULL,  /*   branch ANYA */
    0x380022c0bb703000ULL,  /*   nop */
    0x0be4958cf9581643ULL,  /*   mov */
    0x0bf003d6f93c12c3ULL,  /*   mov */
    0x040007de387d3792ULL,  /*   add */
    0x081007de3c7d1790ULL,  /*   sub */
    0x380000193803f65bULL,  /*   add */
    0x380800193c03f64eULL,  /*   sub */
    0x39e0001c7e03f78fULL,  /*   asr */
    0x05e0071d7e7047cfULL,  /*   asr */
    0x05e0075c79745700ULL,  /*   max */
    0x3800001c7803f706ULL,  /*   min */
    0x39e0001d7903f740ULL,  /*   max */
    0x3800001d7803f747ULL,  /*   min */
    0x25e047583c7483c0ULL,  /*   sub */
    0x3800001d3803f75cULL,  /*   add */
    0x39e0001d7c03f742ULL,  /*   shl */
    0x3800001d3803f74aULL,  /*   add */
    0x02ffff5dff00d000ULL,  /*   branch ANYNA */
    0x3800100cf903f743ULL,  /*   mov */
    0x38870000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x3800100bf903f703ULL,  /*   mov */
    0x3800100cf903f343ULL,  /*   mov */
    0x38001006bb03f00fULL,  /*   tmuwt */
    0x38201012bb03f00eULL,  /*   barrierid */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38200000bb03f000ULL,  /*   nop */
    0x38200000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38200000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x380022c0bb703000ULL,  /*   nop */
    0x0be4958cf9581643ULL,  /*   mov */
    0x0bf003d6f93c12c3ULL,  /*   mov */
    0x040007de387d3792ULL,  /*   add */
    0x081007de3c7d1790ULL,  /*   sub */
    0x380000193803f65bULL,  /*   add */
    0x380800193c03f64eULL,  /*   sub */
    0x39e0001c7e03f78fULL,  /*   asr */
    0x05e0071d7e7047cfULL,  /*   asr */
    0x3800001d3803f745ULL,  /*   add */
    0x25e047583c7483c0ULL,  /*   sub */
    0x3800001d3803f75cULL,  /*   add */
    0x39e0001d7c03f742ULL,  /*   shl */
    0x3800001d3803f74aULL,  /*   add */
    0x02ffff75ff00d000ULL,  /*   branch ANYNA */
    0x3800100cf903f743ULL,  /*   mov */
    0x38870000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x3800100bf903f703ULL,  /*   mov */
    0x3800100cf903f343ULL,  /*   mov */
    0x38001006bb03f00fULL,  /*   tmuwt */
    0x38201012bb03f00eULL,  /*   barrierid */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38200000bb03f000ULL,  /*   nop */
    0x38200000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38200000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
};
const unsigned g2d_qpu_argb_blit_n = 220;

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
const uint64_t g2d_qpu_argb_copy[] = {
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
    0x38000011bb03f002ULL,  /*   eidx */
    0x39e0000b7c03f444ULL,  /*   shl */
    0x3800000a3803f28bULL,  /*   add */
    0x39c0000ff903f703ULL,  /*   mov */
    0x39c00010f903f043ULL,  /*   mov */
    0x39e000107c03f408ULL,  /*   shl */
    0x38000011bb03f001ULL,  /*   tidx */
    0x39e000117d03f442ULL,  /*   shr */
    0x39e00011b503f44fULL,  /*   and */
    0x39e000123803f441ULL,  /*   add */
    0x39e040123c03f481ULL,  /*   sub */
    0x02ffffddff00d000ULL,  /*   branch ANYNA */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x39e000127c03f448ULL,  /*   shl */
    0x3800000a3803f292ULL,  /*   add */
    0x24000480bb448000ULL,  /*   nop */
    0x380000003803f012ULL,  /*   add */
    0x24000480bb449000ULL,  /*   nop */
    0x380000013803f052ULL,  /*   add */
    0x24000480bb447000ULL,  /*   nop */
    0x380000123c03f192ULL,  /*   sub */
    0x39e080133c03f481ULL,  /*   sub */
    0x020001a60000d000ULL,  /*   branch ANYA */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x380000067803f1d2ULL,  /*   min */
    0x38200000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x3800000d3803f00bULL,  /*   add */
    0x3800000e3803f04bULL,  /*   add */
    0x380000003803f002ULL,  /*   add */
    0x380000013803f043ULL,  /*   add */
    0x38001020f903f3c3ULL,  /*   mov */
    0x3800100cf903f383ULL,  /*   mov */
    0x3800000e3803f390ULL,  /*   add */
    0x3800000cf903f103ULL,  /*   mov */
    0x39e040113c03f100ULL,  /*   sub */
    0x0200008c0000d000ULL,  /*   branch ALLA */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38001020f903f3c3ULL,  /*   mov */
    0x3800100cf903f383ULL,  /*   mov */
    0x3800000e3803f390ULL,  /*   add */
    0x38850000bb03f000ULL,  /*   nop */
    0x38854000bb03f000ULL,  /*   nop */
    0x38858000bb03f000ULL,  /*   nop */
    0x3885c000bb03f000ULL,  /*   nop */
    0x38001020f903f3c3ULL,  /*   mov */
    0x3800100bf903f503ULL,  /*   mov */
    0x3800100bf903f543ULL,  /*   mov */
    0x3800100bf903f583ULL,  /*   mov */
    0x3800100bf903f5c3ULL,  /*   mov */
    0x39e0400c3c03f301ULL,  /*   sub */
    0x02ffff7dff00d000ULL,  /*   branch ANYNA */
    0x3800100cf903f343ULL,  /*   mov */
    0x3800000d3803f350ULL,  /*   add */
    0x38000000bb03f000ULL,  /*   nop */
    0x38850000bb03f000ULL,  /*   nop */
    0x38854000bb03f000ULL,  /*   nop */
    0x38858000bb03f000ULL,  /*   nop */
    0x3885c000bb03f000ULL,  /*   nop */
    0x38000018f903f343ULL,  /*   mov */
    0x380080113c03f2c5ULL,  /*   sub */
    0x380a0018f903f283ULL,  /*   mov */
    0x38001020f903f3c3ULL,  /*   mov */
    0x3800100bf903f503ULL,  /*   mov */
    0x3800100bf903f543ULL,  /*   mov */
    0x3800100bf903f583ULL,  /*   mov */
    0x3800100bf903f5c3ULL,  /*   mov */
    0x3800100cf903f603ULL,  /*   mov */
    0x39e040063c03f181ULL,  /*   sub */
    0x02fffe85ff00d000ULL,  /*   branch ANYNA */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38001006bb03f00fULL,  /*   tmuwt */
    0x38201012bb03f00eULL,  /*   barrierid */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38200000bb03f000ULL,  /*   nop */
    0x38200000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38200000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
    0x38000000bb03f000ULL,  /*   nop */
};
const unsigned g2d_qpu_argb_copy_n = 103;

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
