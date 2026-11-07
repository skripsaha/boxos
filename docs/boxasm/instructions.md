# Instructions

BoxASM assembles every instruction x86-64-v3 gives a user program: the
general instructions, x87, MMX, SSE to SSE4.2, AVX, AVX2, FMA, F16C, BMI1,
BMI2 and MOVBE. Each form encodes to the bytes NASM 3 writes for it;
`tools/boxasm_oracle.py` holds the two to that on a corpus of about 36,000
lines (see [internals.md](internals.md#how-it-is-held-to-nasm)). Privileged
instructions (those only the kernel may run) and AVX-512 are not built.

This chapter lists the instructions by family, then what each needs of the
processor, then every mnemonic in one index.

## By family

- Moves and arithmetic: `mov movzx movsx movsxd lea xchg push pop add or adc
  sbb and sub xor cmp test inc dec not neg mul imul div idiv`.
- Shifts and rotates: `rol ror rcl rcr shl sal shr sar shld shrd`.
- Bits: `bt bts btr btc bsf bsr popcnt lzcnt tzcnt bswap`.
- Branches: `jmp call ret`, `jcc setcc cmovcc` with every condition
  spelling, and `loop loope loopz loopne loopnz jrcxz jecxz`, which reach
  only -128 to 127 bytes.
- Strings: `movs stos lods cmps scas`, each with `b w d q`.
- Atomics: `xadd cmpxchg cmpxchg8b cmpxchg16b`; fences `lfence sfence mfence`.
- The processor: `cpuid rdtsc rdtscp rdrand rdseed`.
- SSE and SSE2, which every x86-64 processor has:
  - `add sub mul div min max sqrt` with `ps pd ss sd`; `rcpps rcpss rsqrtps
    rsqrtss`; `and andn or xor unpckl unpckh` with `ps pd`; `shufps shufpd`;
  - compares `cmpps cmppd cmpss cmpsd` with an imm8, and `cmp` + `eq lt le
    unord neq nlt nle ord` + `ps pd ss sd` (`cmpltsd`); `comiss comisd
    ucomiss ucomisd`;
  - moves `movaps movapd movups movupd movss movsd movlps movhps movlpd
    movhpd movhlps movlhps movdqa movdqu movd movq movmskps movmskpd
    pmovmskb movntps movntpd movntdq movnti maskmovdqu`;
  - conversions `cvtsi2ss cvtsi2sd cvtss2si cvttss2si cvtsd2si cvttsd2si
    cvtss2sd cvtsd2ss cvtps2pd cvtpd2ps cvtdq2ps cvtps2dq cvttps2dq cvtdq2pd
    cvtpd2dq cvttpd2dq`;
  - packed integers `padd psub` (`b w d q`), `padds paddus psubs psubus`
    (`b w`), `pmullw pmulhw pmulhuw pmuludq pmaddwd pavgb pavgw pmaxsw
    pmaxub pminsw pminub psadbw pand pandn por pxor pcmpeq pcmpgt` (`b w d`),
    `packsswb packssdw packuswb punpckl punpckh` (`bw wd dq qdq`), shifts
    `psll psrl` (`w d q`) and `psra` (`w d`) by xmm or imm8, `pslldq psrldq`,
    `pshufd pshufhw pshuflw pextrw pinsrw`;
  - `ldmxcsr stmxcsr prefetchnta prefetcht0 prefetcht1 prefetcht2 clflush`.

  `movsd` and `cmpsd` without operands are the string instructions; with
  them, SSE2's. An SSE operand of memory reads 4 (`ss`), 8 (`sd`, `movq`) or
  16 bytes (`oword`); a size written must be that one.

  Packed operations need their 16 bytes of memory on a 16-byte boundary, or
  the processor stops the program; only `movups movupd movdqu` read memory
  anywhere. When the memory is a label of the program, the assembler proves
  the boundary — the label's place a multiple of 16 in a section something
  said `align 16` in — or refuses the line. Memory through a register is the
  program's to align.

  The C call ABI passes `double` and `float` arguments in `xmm0..xmm7` and
  returns one in `xmm0`; every xmm register may change across a call, and
  `al` counts the vector registers a variadic callee receives. A `float`
  goes to a variadic function such as `printf` widened to a `double`
  (`cvtss2sd`). BoxLib's `printf` writes C's whole set of conversions, `%f
  %e %g %a` among them, each value's digits exact and rounded to nearest:
  `fractions.asm` prints its results that way.
- SSE3, SSSE3, SSE4.1 and SSE4.2:
  - `addsubps addsubpd haddps haddpd hsubps hsubpd lddqu movddup movshdup
    movsldup monitor mwait`;
  - `pabsb pabsw pabsd phaddw phaddd phaddsw phsubw phsubd phsubsw pmaddubsw
    pmulhrsw pshufb psignb psignw psignd palignr`;
  - `blendps blendpd pblendw` and `blendvps blendvpd pblendvb`, whose mask is
    xmm0, written third or left out; `dpps dppd mpsadbw extractps insertps
    movntdqa packusdw pcmpeqq phminposuw ptest pmuldq pmulld`, `pmin` and
    `pmax` with `sb sd uw ud`, `pextrb pextrd pextrq pinsrb pinsrd pinsrq`,
    `pmovsx` and `pmovzx` with `bw bd bq wd wq dq`, `roundps roundpd roundss
    roundsd`;
  - `crc32` of a byte, word, dword or qword, `pcmpestri pcmpestrm pcmpistri
    pcmpistrm pcmpgtq`.
- AVX and AVX2, in the VEX encoding:
  - the `v` form of each SSE to SSE4.2 instruction above but `movnti`, the
    prefetches, `clflush`, `monitor`, `mwait` and `crc32`, which have none,
    with a destination of its own: `vaddps
    xmm1, xmm2, xmm3`. Where the second operand is left out it is the first
    again: `vaddps xmm1, xmm2` is `vaddps xmm1, xmm1, xmm2`. AVX widens the
    floating-point operations and the moves to ymm, AVX2 the integer ones;
    the others stay on xmm and say so when given a ymm;
  - `vcmpps vcmppd vcmpss vcmpsd` with an imm8, and `vcmp`, one of the 32
    predicates by name, then `ps pd ss sd`: `vcmplt_oqpd`, `vcmpneq_uqss`;
  - AVX's own: `vbroadcastss vbroadcastsd vbroadcastf128 vextractf128
    vinsertf128 vmaskmovps vmaskmovpd vpermilps vpermilpd vperm2f128 vtestps
    vtestpd vzeroupper vzeroall`; `vblendvps vblendvpd vpblendvb` take the
    register they select by last;
  - AVX2's own: `vpbroadcastb vpbroadcastw vpbroadcastd vpbroadcastq
    vbroadcasti128 vextracti128 vinserti128 vperm2i128 vpermd vpermps vpermq
    vpermpd vpblendd vpmaskmovd vpmaskmovq vpsllvd vpsllvq vpsravd vpsrlvd
    vpsrlvq`;
  - the gathers `vgatherdps vgatherdpd vgatherqps vgatherqpd vpgatherdd
    vpgatherdq vpgatherqd vpgatherqq`, whose address is indexed by a vector
    register: `vpgatherdd ymm0, [rax + ymm1*4], ymm2`. The destination, the
    index and the mask are three different registers, or the processor stops
    the program, so the assembler refuses them otherwise. A vector register
    indexes no other address.

  Of AVX's memory operands only those of `vmovaps vmovapd vmovdqa` and the
  non-temporal moves must be on a boundary of their width, 16 or 32 bytes;
  the assembler proves it as it does for SSE.
- FMA: `vfmadd vfmsub vfnmadd vfnmsub` with `132 213 231` and `ps pd ss sd`,
  and `vfmaddsub vfmsubadd` with `132 213 231` and `ps pd` — a product and a
  sum rounded once. The digits say which operands are multiplied: 132 is
  `first = first * third + second`, 213 is `first = second * first + third`,
  231 is `first = second * third + first`. All three are always written.
- F16C: `vcvtph2ps` turns four or eight half-precision numbers into singles,
  `vcvtps2ph` turns them back, rounded as its imm8 says.
- BMI1 and BMI2, on 32- and 64-bit registers: `andn bextr blsi blsmsk blsr
  bzhi mulx pdep pext rorx sarx shlx shrx`. Of those with three operands the
  second may be left out: `shlx eax, ecx` is `shlx eax, eax, ecx`.
- `movbe` loads or stores with the bytes reversed; `lahf` and `sahf` move the
  flags through ah; `xgetbv` reads the register ecx names — with 0, XCR0,
  whose bits 1 and 2 say the system saves the xmm and ymm registers.
- x87, on the stack of st0 (its top) to st7:
  - loads and stores: `fld fst fstp` of memory (`dword` or `qword`, and
    `tword` but for `fst`) or a stack register, `fild fistp fisttp` of a
    `word`, `dword` or `qword` integer and `fist` of a `word` or `dword`,
    `fbld fbstp` of a 10-byte decimal, `fxch`, and the constants `fld1 fldz
    fldpi fldl2e fldl2t fldlg2 fldln2`;
  - `fadd fsub fsubr fmul fdiv fdivr` of a `dword` or `qword` of memory into
    st0, or between st0 and st(i) either way (`fadd st0, st3`, `fadd st3,
    st0`, and `fadd st3` for st0 += st3); their popping forms `faddp` and
    kin; and `fiadd fisub fisubr fimul fidiv fidivr` of a `word` or `dword`
    integer;
  - compares `fcom fcomp fcompp fucom fucomp fucompp ficom ficomp ftst fxam`,
    and `fcomi fcomip fucomi fucomip`, which set the flags `jcc` reads, and
    the moves on them `fcmovb fcmove fcmovbe fcmovu fcmovnb fcmovne fcmovnbe
    fcmovnu`;
  - `fabs fchs fsqrt fsin fcos fsincos fptan fpatan f2xm1 fyl2x fyl2xp1
    fscale fprem fprem1 frndint fxtract fdecstp fincstp ffree ffreep fnop`;
  - the control word `fldcw fstcw fnstcw`, the status word `fstsw fnstsw` (to
    `ax` or a word of memory), the environment and state `fldenv fstenv
    fnstenv frstor fsave fnsave`, `finit fninit fclex fnclex fwait wait`;
    `fxsave fxrstor fxsave64 fxrstor64` keep the x87, MMX and SSE state in
    512 bytes on a 16-byte boundary, proven for a label as for SSE.

  A stack register is written st0 to st7. Left out, a register is st1:
  `fxch`, `fcom`, `fld`, `fcmovb` are those with st1, and `fadd` with no
  operand is `faddp st1, st0`, as NASM takes them. The waiting forms
  (`fstcw`, `fstsw`, `finit`) are `wait` before the form that does not wait.
  Memory an instruction reads in several sizes says which; memory of one
  size may say it or not, and the environment and state say none.
- MMX, on `mm0..mm7`: the integer operations of SSE2 and SSSE3 above, each
  also on an mm register and mm/m64 (`paddw mm1, [rax]`, `pshufb mm0, mm1`,
  `psllq mm2, 4`) but `punpcklqdq punpckhqdq pshufd pshufhw pshuflw pslldq
  psrldq`; `punpcklbw punpcklwd punpckldq` of mm read 4 bytes of memory.
  `movd` (32 bits) and `movq` (64) between mm and a general register or
  memory, `movq` between mm registers, `pextrw pinsrw pmovmskb` with a
  32-bit register, `pshufw maskmovq movntq emms`; and SSE's `cvtpi2ps
  cvtpi2pd cvtps2pi cvttps2pi cvtpd2pi cvttpd2pi movq2dq movdq2q`. The mm
  registers are the x87 registers' own: `emms` gives them back to the x87.
- Alone: `nop int3 leave cqo cdq cwd cdqe cwde cbw pushfq popfq` (also
  `pushf popf`) `cld std clc stc cmc endbr64 pause ud2`.

## Processor features

`popcnt` needs CPUID.1:ECX bit 23, `lzcnt` the LZCNT bit of leaf 0x80000001,
`tzcnt` BMI1, `rdrand` and `rdseed` their own bits, `cmpxchg16b` CX16. So
does each set: SSE3, SSSE3, SSE4.1, SSE4.2, MOVBE, FMA, AVX and F16C in
CPUID.1:ECX (bits 0, 9, 19, 20, 22, 12, 28, 29), AVX2, BMI1 and BMI2 in
CPUID.7:EBX (bits 5, 3, 8), `lahf` and `sahf` in bit 0 of ECX of leaf
0x80000001. An instruction encoded with VEX also needs the system to save
the ymm registers: CPUID.1:ECX bit 27 (OSXSAVE), then `xgetbv` with ecx 0
and bits 1 and 2 of eax. BoxOS saves them. x87, MMX and fxsave are in
every x86-64 processor.

The assembler cannot know the machine a program will run on: a processor
without `lzcnt` or `tzcnt` runs them as `bsr` and `bsf`, which answer
differently, and one without the others stops the program. Ask `cpuid`
first, as `machine.asm` does with `bt ecx, 23`.

| Set | CPUID | Notes |
|---|---|---|
| x87, MMX, SSE, SSE2, fxsave, cmov, cmpxchg8b | always on x86-64 | the baseline |
| SSE3, SSSE3, SSE4.1, SSE4.2 | leaf 1 ECX bits 0, 9, 19, 20 | x86-64-v2 |
| POPCNT, CMPXCHG16B | leaf 1 ECX bits 23, 13 | x86-64-v2 |
| LAHF/SAHF | leaf 0x80000001 ECX bit 0 | x86-64-v2 |
| AVX, F16C, FMA, MOVBE | leaf 1 ECX bits 28, 29, 12, 22 | x86-64-v3; AVX also needs OSXSAVE (ECX bit 27) and XCR0 bits 1–2 |
| AVX2, BMI1, BMI2 | leaf 7 EBX bits 5, 3, 8 | x86-64-v3 |
| LZCNT | leaf 0x80000001 ECX bit 5 | x86-64-v3 |
| RDRAND, RDSEED | leaf 1 ECX bit 30; leaf 7 EBX bit 18 | |

`machine.asm` among the examples asks `cpuid` before it uses `popcnt`. In
QEMU, BoxOS runs on `qemu64` unless started with `STRICT=on`, which gives
`-cpu max`: SSSE3 and later need it.

## Index

Every mnemonic BoxASM takes, but the condition and predicate spellings:
`jcc`, `setcc` and `cmovcc` take every condition (`o no b c nae ae nb nc e z
ne nz be na a nbe s ns p pe np po l nge ge nl le ng g nle`), `cmp` + `eq lt
le unord neq nlt nle ord` + `ps pd ss sd` names SSE's eight predicates, and
`vcmp` + one of AVX's 32 (`eq lt le unord neq nlt nle ord eq_uq nge ngt
false neq_oq ge gt true eq_os lt_oq le_oq unord_s neq_us nlt_uq nle_uq ord_s
eq_us nge_uq ngt_uq false_os neq_os ge_oq gt_oq true_us`, and the `_os` /
`_q` / `_us` spellings of the first sixteen) + `ps pd ss sd` names those.

**A** — `adc` `add` `addpd` `addps` `addsd` `addss` `addsubpd` `addsubps` `and` `andn` `andnpd` `andnps` `andpd` `andps`

**B** — `bextr` `blendpd` `blendps` `blendvpd` `blendvps` `blsi` `blsmsk` `blsr` `bsf` `bsr` `bswap` `bt` `btc` `btr` `bts` `bzhi`

**C** — `call` `cbw` `cdq` `cdqe` `clc` `cld` `clflush` `cmc` `cmp` `cmppd` `cmpps` `cmpsb` `cmpsd` `cmpsq` `cmpss` `cmpsw` `cmpxchg` `cmpxchg16b` `cmpxchg8b` `comisd` `comiss` `cpuid` `cqo` `crc32` `cvtdq2pd` `cvtdq2ps` `cvtpd2dq` `cvtpd2pi` `cvtpd2ps` `cvtpi2pd` `cvtpi2ps` `cvtps2dq` `cvtps2pd` `cvtps2pi` `cvtsd2si` `cvtsd2ss` `cvtsi2sd` `cvtsi2ss` `cvtss2sd` `cvtss2si` `cvttpd2dq` `cvttpd2pi` `cvttps2dq` `cvttps2pi` `cvttsd2si` `cvttss2si` `cwd` `cwde`

**D** — `dec` `div` `divpd` `divps` `divsd` `divss` `dppd` `dpps`

**E** — `emms` `endbr64` `extractps`

**F** — `f2xm1` `fabs` `fadd` `faddp` `fbld` `fbstp` `fchs` `fclex` `fcmovb` `fcmovbe` `fcmove` `fcmovnb` `fcmovnbe` `fcmovne` `fcmovnu` `fcmovu` `fcom` `fcomi` `fcomip` `fcomp` `fcompp` `fcos` `fdecstp` `fdiv` `fdivp` `fdivr` `fdivrp` `ffree` `ffreep` `fiadd` `ficom` `ficomp` `fidiv` `fidivr` `fild` `fimul` `fincstp` `finit` `fist` `fistp` `fisttp` `fisub` `fisubr` `fld` `fld1` `fldcw` `fldenv` `fldl2e` `fldl2t` `fldlg2` `fldln2` `fldpi` `fldz` `fmul` `fmulp` `fnclex` `fninit` `fnop` `fnsave` `fnstcw` `fnstenv` `fnstsw` `fpatan` `fprem` `fprem1` `fptan` `frndint` `frstor` `fsave` `fscale` `fsin` `fsincos` `fsqrt` `fst` `fstcw` `fstenv` `fstp` `fstsw` `fsub` `fsubp` `fsubr` `fsubrp` `ftst` `fucom` `fucomi` `fucomip` `fucomp` `fucompp` `fwait` `fxam` `fxch` `fxrstor` `fxrstor64` `fxsave` `fxsave64` `fxtract` `fyl2x` `fyl2xp1`

**H** — `haddpd` `haddps` `hsubpd` `hsubps`

**I** — `idiv` `imul` `inc` `insertps` `int3`

**J** — `jecxz` `jmp` `jrcxz`

**L** — `lahf` `lddqu` `ldmxcsr` `lea` `leave` `lfence` `lodsb` `lodsd` `lodsq` `lodsw` `loop` `loope` `loopne` `loopnz` `loopz` `lzcnt`

**M** — `maskmovdqu` `maskmovq` `maxpd` `maxps` `maxsd` `maxss` `mfence` `minpd` `minps` `minsd` `minss` `monitor` `mov` `movapd` `movaps` `movbe` `movd` `movddup` `movdq2q` `movdqa` `movdqu` `movhlps` `movhpd` `movhps` `movlhps` `movlpd` `movlps` `movmskpd` `movmskps` `movntdq` `movntdqa` `movnti` `movntpd` `movntps` `movntq` `movq` `movq2dq` `movsb` `movsd` `movshdup` `movsldup` `movsq` `movss` `movsw` `movsx` `movsxd` `movupd` `movups` `movzx` `mpsadbw` `mul` `mulpd` `mulps` `mulsd` `mulss` `mulx` `mwait`

**N** — `neg` `nop` `not`

**O** — `or` `orpd` `orps`

**P** — `pabsb` `pabsd` `pabsw` `packssdw` `packsswb` `packusdw` `packuswb` `paddb` `paddd` `paddq` `paddsb` `paddsw` `paddusb` `paddusw` `paddw` `palignr` `pand` `pandn` `pause` `pavgb` `pavgw` `pblendvb` `pblendw` `pcmpeqb` `pcmpeqd` `pcmpeqq` `pcmpeqw` `pcmpestri` `pcmpestrm` `pcmpgtb` `pcmpgtd` `pcmpgtq` `pcmpgtw` `pcmpistri` `pcmpistrm` `pdep` `pext` `pextrb` `pextrd` `pextrq` `pextrw` `phaddd` `phaddsw` `phaddw` `phminposuw` `phsubd` `phsubsw` `phsubw` `pinsrb` `pinsrd` `pinsrq` `pinsrw` `pmaddubsw` `pmaddwd` `pmaxsb` `pmaxsd` `pmaxsw` `pmaxub` `pmaxud` `pmaxuw` `pminsb` `pminsd` `pminsw` `pminub` `pminud` `pminuw` `pmovmskb` `pmovsxbd` `pmovsxbq` `pmovsxbw` `pmovsxdq` `pmovsxwd` `pmovsxwq` `pmovzxbd` `pmovzxbq` `pmovzxbw` `pmovzxdq` `pmovzxwd` `pmovzxwq` `pmuldq` `pmulhrsw` `pmulhuw` `pmulhw` `pmulld` `pmullw` `pmuludq` `pop` `popcnt` `popf` `popfq` `por` `prefetchnta` `prefetcht0` `prefetcht1` `prefetcht2` `psadbw` `pshufb` `pshufd` `pshufhw` `pshuflw` `pshufw` `psignb` `psignd` `psignw` `pslld` `pslldq` `psllq` `psllw` `psrad` `psraw` `psrld` `psrldq` `psrlq` `psrlw` `psubb` `psubd` `psubq` `psubsb` `psubsw` `psubusb` `psubusw` `psubw` `ptest` `punpckhbw` `punpckhdq` `punpckhqdq` `punpckhwd` `punpcklbw` `punpckldq` `punpcklqdq` `punpcklwd` `push` `pushf` `pushfq` `pxor`

**R** — `rcl` `rcpps` `rcpss` `rcr` `rdrand` `rdseed` `rdtsc` `rdtscp` `ret` `rol` `ror` `rorx` `roundpd` `roundps` `roundsd` `roundss` `rsqrtps` `rsqrtss`

**S** — `sahf` `sal` `sar` `sarx` `sbb` `scasb` `scasd` `scasq` `scasw` `sfence` `shl` `shld` `shlx` `shr` `shrd` `shrx` `shufpd` `shufps` `sqrtpd` `sqrtps` `sqrtsd` `sqrtss` `stc` `std` `stmxcsr` `stosb` `stosd` `stosq` `stosw` `sub` `subpd` `subps` `subsd` `subss`

**T** — `test` `tzcnt`

**U** — `ucomisd` `ucomiss` `ud2` `unpckhpd` `unpckhps` `unpcklpd` `unpcklps`

**V** — `vaddpd` `vaddps` `vaddsd` `vaddss` `vaddsubpd` `vaddsubps` `vandnpd` `vandnps` `vandpd` `vandps` `vblendpd` `vblendps` `vblendvpd` `vblendvps` `vbroadcastf128` `vbroadcasti128` `vbroadcastsd` `vbroadcastss` `vcmppd` `vcmpps` `vcmpsd` `vcmpss` `vcomisd` `vcomiss` `vcvtdq2pd` `vcvtdq2ps` `vcvtpd2dq` `vcvtpd2ps` `vcvtph2ps` `vcvtps2dq` `vcvtps2pd` `vcvtps2ph` `vcvtsd2si` `vcvtsd2ss` `vcvtsi2sd` `vcvtsi2ss` `vcvtss2sd` `vcvtss2si` `vcvttpd2dq` `vcvttps2dq` `vcvttsd2si` `vcvttss2si` `vdivpd` `vdivps` `vdivsd` `vdivss` `vdppd` `vdpps` `vextractf128` `vextracti128` `vextractps` `vfmadd132pd` `vfmadd132ps` `vfmadd132sd` `vfmadd132ss` `vfmadd213pd` `vfmadd213ps` `vfmadd213sd` `vfmadd213ss` `vfmadd231pd` `vfmadd231ps` `vfmadd231sd` `vfmadd231ss` `vfmaddsub132pd` `vfmaddsub132ps` `vfmaddsub213pd` `vfmaddsub213ps` `vfmaddsub231pd` `vfmaddsub231ps` `vfmsub132pd` `vfmsub132ps` `vfmsub132sd` `vfmsub132ss` `vfmsub213pd` `vfmsub213ps` `vfmsub213sd` `vfmsub213ss` `vfmsub231pd` `vfmsub231ps` `vfmsub231sd` `vfmsub231ss` `vfmsubadd132pd` `vfmsubadd132ps` `vfmsubadd213pd` `vfmsubadd213ps` `vfmsubadd231pd` `vfmsubadd231ps` `vfnmadd132pd` `vfnmadd132ps` `vfnmadd132sd` `vfnmadd132ss` `vfnmadd213pd` `vfnmadd213ps` `vfnmadd213sd` `vfnmadd213ss` `vfnmadd231pd` `vfnmadd231ps` `vfnmadd231sd` `vfnmadd231ss` `vfnmsub132pd` `vfnmsub132ps` `vfnmsub132sd` `vfnmsub132ss` `vfnmsub213pd` `vfnmsub213ps` `vfnmsub213sd` `vfnmsub213ss` `vfnmsub231pd` `vfnmsub231ps` `vfnmsub231sd` `vfnmsub231ss` `vgatherdpd` `vgatherdps` `vgatherqpd` `vgatherqps` `vhaddpd` `vhaddps` `vhsubpd` `vhsubps` `vinsertf128` `vinserti128` `vinsertps` `vlddqu` `vldmxcsr` `vmaskmovdqu` `vmaskmovpd` `vmaskmovps` `vmaxpd` `vmaxps` `vmaxsd` `vmaxss` `vminpd` `vminps` `vminsd` `vminss` `vmovapd` `vmovaps` `vmovd` `vmovddup` `vmovdqa` `vmovdqu` `vmovhlps` `vmovhpd` `vmovhps` `vmovlhps` `vmovlpd` `vmovlps` `vmovmskpd` `vmovmskps` `vmovntdq` `vmovntdqa` `vmovntpd` `vmovntps` `vmovq` `vmovsd` `vmovshdup` `vmovsldup` `vmovss` `vmovupd` `vmovups` `vmpsadbw` `vmulpd` `vmulps` `vmulsd` `vmulss` `vorpd` `vorps` `vpabsb` `vpabsd` `vpabsw` `vpackssdw` `vpacksswb` `vpackusdw` `vpackuswb` `vpaddb` `vpaddd` `vpaddq` `vpaddsb` `vpaddsw` `vpaddusb` `vpaddusw` `vpaddw` `vpalignr` `vpand` `vpandn` `vpavgb` `vpavgw` `vpblendd` `vpblendvb` `vpblendw` `vpbroadcastb` `vpbroadcastd` `vpbroadcastq` `vpbroadcastw` `vpcmpeqb` `vpcmpeqd` `vpcmpeqq` `vpcmpeqw` `vpcmpestri` `vpcmpestrm` `vpcmpgtb` `vpcmpgtd` `vpcmpgtq` `vpcmpgtw` `vpcmpistri` `vpcmpistrm` `vperm2f128` `vperm2i128` `vpermd` `vpermilpd` `vpermilps` `vpermpd` `vpermps` `vpermq` `vpextrb` `vpextrd` `vpextrq` `vpextrw` `vpgatherdd` `vpgatherdq` `vpgatherqd` `vpgatherqq` `vphaddd` `vphaddsw` `vphaddw` `vphminposuw` `vphsubd` `vphsubsw` `vphsubw` `vpinsrb` `vpinsrd` `vpinsrq` `vpinsrw` `vpmaddubsw` `vpmaddwd` `vpmaskmovd` `vpmaskmovq` `vpmaxsb` `vpmaxsd` `vpmaxsw` `vpmaxub` `vpmaxud` `vpmaxuw` `vpminsb` `vpminsd` `vpminsw` `vpminub` `vpminud` `vpminuw` `vpmovmskb` `vpmovsxbd` `vpmovsxbq` `vpmovsxbw` `vpmovsxdq` `vpmovsxwd` `vpmovsxwq` `vpmovzxbd` `vpmovzxbq` `vpmovzxbw` `vpmovzxdq` `vpmovzxwd` `vpmovzxwq` `vpmuldq` `vpmulhrsw` `vpmulhuw` `vpmulhw` `vpmulld` `vpmullw` `vpmuludq` `vpor` `vpsadbw` `vpshufb` `vpshufd` `vpshufhw` `vpshuflw` `vpsignb` `vpsignd` `vpsignw` `vpslld` `vpslldq` `vpsllq` `vpsllvd` `vpsllvq` `vpsllw` `vpsrad` `vpsravd` `vpsraw` `vpsrld` `vpsrldq` `vpsrlq` `vpsrlvd` `vpsrlvq` `vpsrlw` `vpsubb` `vpsubd` `vpsubq` `vpsubsb` `vpsubsw` `vpsubusb` `vpsubusw` `vpsubw` `vptest` `vpunpckhbw` `vpunpckhdq` `vpunpckhqdq` `vpunpckhwd` `vpunpcklbw` `vpunpckldq` `vpunpcklqdq` `vpunpcklwd` `vpxor` `vrcpps` `vrcpss` `vroundpd` `vroundps` `vroundsd` `vroundss` `vrsqrtps` `vrsqrtss` `vshufpd` `vshufps` `vsqrtpd` `vsqrtps` `vsqrtsd` `vsqrtss` `vstmxcsr` `vsubpd` `vsubps` `vsubsd` `vsubss` `vtestpd` `vtestps` `vucomisd` `vucomiss` `vunpckhpd` `vunpckhps` `vunpcklpd` `vunpcklps` `vxorpd` `vxorps` `vzeroall` `vzeroupper`

**W** — `wait`

**X** — `xadd` `xchg` `xgetbv` `xor` `xorpd` `xorps`
