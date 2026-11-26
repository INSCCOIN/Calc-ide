/* ops.h - CalcIDE bytecode opcodes.
 *
 * Shared by the compiler (cc.c, which is written in the Tiny-C dialect and
 * compiles itself) and the virtual machine (src/vm.c). Only plain #define
 * lines live here so that both compilers can read it.
 *
 * Operand encodings (little endian, unaligned):
 *   i8 / i16 / i32 : signed immediates
 *   a32            : absolute code address
 */
#ifndef CALCIDE_OPS_H
#define CALCIDE_OPS_H

#define CBC_MAGIC     0x31434243   /* "CBC1" */
#define CBC_VERSION   2
#define CBC_DATA_BASE 64           /* first data address; below is a NULL trap */

#define OP_HALT    0
#define OP_IMM     1   /* i32 */
#define OP_IMM8    2   /* i8  */
#define OP_IMM16   3   /* i16 */
#define OP_LEA     4   /* i16 : push fp + off */
#define OP_LEA32   5   /* i32 */
#define OP_LDB     6
#define OP_LDBU    7
#define OP_LDH     8
#define OP_LDHU    9
#define OP_LDW     10
#define OP_STB     11
#define OP_STH     12
#define OP_STW     13
#define OP_ADD     14
#define OP_SUB     15
#define OP_MUL     16
#define OP_DIV     17
#define OP_MOD     18
#define OP_DIVU    19
#define OP_MODU    20
#define OP_AND     21
#define OP_OR      22
#define OP_XOR     23
#define OP_SHL     24
#define OP_SHR     25
#define OP_SAR     26
#define OP_NEG     27
#define OP_NOT     28
#define OP_LNOT    29
#define OP_EQ      30
#define OP_NE      31
#define OP_LT      32
#define OP_LE      33
#define OP_GT      34
#define OP_GE      35
#define OP_LTU     36
#define OP_LEU     37
#define OP_GTU     38
#define OP_GEU     39
#define OP_FADD    40
#define OP_FSUB    41
#define OP_FMUL    42
#define OP_FDIV    43
#define OP_FNEG    44
#define OP_FEQ     45
#define OP_FNE     46
#define OP_FLT     47
#define OP_FLE     48
#define OP_FGT     49
#define OP_FGE     50
#define OP_ITOF    51
#define OP_UTOF    52
#define OP_FTOI    53
#define OP_FTOU    54
#define OP_ITOF2   55  /* convert the cell below the top */
#define OP_UTOF2   56
#define OP_SEXT8   57
#define OP_ZEXT8   58
#define OP_SEXT16  59
#define OP_ZEXT16  60
#define OP_JMP     61  /* a32 */
#define OP_JZ      62  /* a32 : pop, jump if zero */
#define OP_JNZ     63  /* a32 */
#define OP_CALL    64  /* a32 */
#define OP_CALLI   65  /* n8 : target sits below the n arguments */
#define OP_ENTER   66  /* n8 parameter cells, i32 frame size */
#define OP_RET     67  /* returns 0 */
#define OP_RETV    68  /* returns top of stack */
#define OP_POP     69
#define OP_DUP     70
#define OP_SWAP    71
#define OP_MEMCPY  72  /* i32 n : pop src, pop dst, copy, push dst */
#define OP_SYS     73  /* i16 id (| 0x8000: returns 2 cells), n8 arg cells */
#define OP_INC     74  /* m8 mode, i32 delta : pop addr, push old/new */
#define OP_LLW     75  /* i16 : push word at fp + off */
#define OP_MEMSET  76  /* i32 n : pop addr, zero n bytes */
#define OP_ADDI    77  /* i32 : top += imm */
#define OP_LGW     78  /* i32 : push word at absolute address */
/* 64-bit values (long long, double) occupy two cells: low word, then high
 * word on top. */
#define OP_LDD     79  /* load 8 bytes */
#define OP_STD     80  /* [addr lo hi] -> store, leaves [lo hi] */
#define OP_LADD    81
#define OP_LSUB    82
#define OP_LMUL    83
#define OP_LDIV    84
#define OP_LMOD    85
#define OP_LDIVU   86
#define OP_LMODU   87
#define OP_LAND    88
#define OP_LOR     89
#define OP_LXOR    90
#define OP_LSHL    91  /* [lo hi n] */
#define OP_LSHR    92
#define OP_LSAR    93
#define OP_LNEG    94
#define OP_LCOM    95  /* bitwise not */
#define OP_LEQ     96  /* 64-bit compares leave one int cell */
#define OP_LNE     97
#define OP_LLT     98
#define OP_LLE     99
#define OP_LGT     100
#define OP_LGE     101
#define OP_LLTU    102
#define OP_LLEU    103
#define OP_LGTU    104
#define OP_LGEU    105
#define OP_DADD    106
#define OP_DSUB    107
#define OP_DMUL    108
#define OP_DDIV    109
#define OP_DNEG    110
#define OP_DEQ     111
#define OP_DNE     112
#define OP_DLT     113
#define OP_DLE     114
#define OP_DGT     115
#define OP_DGE     116
#define OP_I2L     117  /* conversions */
#define OP_U2L     118
#define OP_L2I     119
#define OP_I2D     120
#define OP_U2D     121
#define OP_L2D     122
#define OP_UL2D    123
#define OP_F2D     124
#define OP_D2F     125
#define OP_D2I     126
#define OP_D2U     127
#define OP_D2L     128
#define OP_D2UL    129
#define OP_L2B     130  /* 64-bit value != 0 -> int */
#define OP_D2B     131
#define OP_CVTU    132  /* op8 n8 : apply conversion op to the value under n cells */
#define OP_ENTERV  133  /* n8 fixed cells, i32 frame size; variadic: count cell on top */
#define OP_VASTART 134  /* push the address of the variadic arguments */
#define OP_STWP    135  /* store and pop: [addr val] -> [] */
#define OP_STBP    136
#define OP_STHP    137
#define OP_STDP    138
#define OP_JEQ     139  /* a32 : fused compare and branch (pop 2) */
#define OP_JNE     140
#define OP_JLT     141
#define OP_JLE     142
#define OP_JGT     143
#define OP_JGE     144
#define OP_LAST    145

/* OP_INC mode byte: low 3 bits = width kind, bit 3 = postfix */
#define INC_B      0
#define INC_BU     1
#define INC_H      2
#define INC_HU     3
#define INC_W      4
#define INC_F      5
#define INC_L      6   /* 64-bit integer */
#define INC_D      7   /* double */
#define INC_POST   8

#endif
