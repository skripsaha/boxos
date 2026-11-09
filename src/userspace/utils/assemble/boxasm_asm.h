#ifndef BOXASM_ASM_H
#define BOXASM_ASM_H

#include "boxasm_elf.h"
#include "boxasm_lex.h"

/* ---- Registers ---------------------------------------------------------- */

typedef enum {
    BOXASM_REGISTER_NONE = 0,
    BOXASM_REGISTER_GENERAL,    /* rax..r15 and their 32/16/8-bit names */
    BOXASM_REGISTER_HIGH8,      /* ah ch dh bh: no REX may accompany them */
    BOXASM_REGISTER_RIP,
    BOXASM_REGISTER_XMM,        /* xmm0..xmm15 */
    BOXASM_REGISTER_YMM,        /* ymm0..ymm15: the xmm register's 256 bits */
    BOXASM_REGISTER_MMX,        /* mm0..mm7: the x87 registers' 64-bit mantissas */
    BOXASM_REGISTER_FPU         /* st0..st7: the x87 stack, from its top */
} BoxAsmRegisterClass;

typedef struct {
    uint8_t Class;
    uint8_t Number;     /* 0..15 */
    uint8_t Size;       /* bytes: 1, 2, 4, 8, 10, 16, 32 */
    uint8_t NeedsRex;   /* spl bpl sil dil */
} BoxAsmRegister;

/* ---- Sections ----------------------------------------------------------- */

typedef enum {
    BOXASM_SECTION_TEXT = 0,
    BOXASM_SECTION_RODATA,
    BOXASM_SECTION_DATA,
    BOXASM_SECTION_BSS,
    BOXASM_SECTION_COUNT
} BoxAsmSection;

/* The most bytes a section, or a struc, may hold. */
#define BOXASM_MAX_SECTION_BYTES (1ull << 32)

/* ---- Expressions -------------------------------------------------------- */

typedef enum {
    BOXASM_EXPR_NUMBER = 1,
    BOXASM_EXPR_SYMBOL,
    BOXASM_EXPR_HERE,           /* $  : the address of the statement */
    BOXASM_EXPR_SECTION_START,  /* $$ : the start of its section */
    BOXASM_EXPR_NEGATE,
    BOXASM_EXPR_COMPLEMENT,
    BOXASM_EXPR_BINARY
} BoxAsmExprKind;

/* An expression node; nodes live in one array and refer to each other by
 * index, 0 meaning none. */
typedef struct {
    uint8_t        Kind;
    uint16_t       Operator;    /* BINARY: + - * / % & | ^ << >> */
    uint32_t       Left;
    uint32_t       Right;
    uint64_t       Number;
    uint32_t       Symbol;
    BoxAsmLocation Where;
} BoxAsmExpr;

/* What an expression is worth: a constant, or a symbol plus a constant. */
typedef struct {
    int64_t  Constant;
    uint32_t Symbol;            /* 0: none */
} BoxAsmValue;

/* ---- Operands ----------------------------------------------------------- */

typedef enum {
    BOXASM_OPERAND_NONE = 0,
    BOXASM_OPERAND_REGISTER,
    BOXASM_OPERAND_IMMEDIATE,
    BOXASM_OPERAND_MEMORY
} BoxAsmOperandKind;

typedef enum {
    BOXASM_BRANCH_ANY = 0,      /* the assembler picks the shortest that reaches */
    BOXASM_BRANCH_SHORT,        /* `short`: rel8 or an error */
    BOXASM_BRANCH_NEAR          /* `near`: rel32 */
} BoxAsmBranchReach;

typedef struct {
    uint8_t        Kind;
    uint8_t        Size;        /* bytes the operand says it is; 0 when it does not */
    uint8_t        Reach;       /* IMMEDIATE used as a branch target */
    BoxAsmRegister Register;    /* REGISTER */
    uint32_t       Expr;        /* IMMEDIATE: the value; MEMORY: displacement, 0 none */
    BoxAsmRegister Base;        /* MEMORY: Class NONE when absent, RIP for rip */
    BoxAsmRegister Index;
    uint8_t        Scale;
    uint8_t        Absolute;    /* `abs`: a symbol-only address is not RIP-relative */
    BoxAsmValue    Value;       /* Expr evaluated for the pass in progress */
    BoxAsmLocation Where;
} BoxAsmOperand;

/* ---- Statements --------------------------------------------------------- */

typedef enum {
    BOXASM_STMT_LABEL = 1,
    BOXASM_STMT_INSTRUCTION,
    BOXASM_STMT_DATA,           /* db dw dd dq */
    BOXASM_STMT_RESERVE,        /* resb resw resd resq */
    BOXASM_STMT_ALIGN,
    BOXASM_STMT_PAD             /* zeros up to Symbol + Count: istruc's at and iend */
} BoxAsmStatementKind;

struct BoxAsmInstruction;
typedef struct BoxAsmExpansion BoxAsmExpansion;

/* A prefix written before an instruction. */
typedef enum {
    BOXASM_PREFIX_NONE = 0,
    BOXASM_PREFIX_LOCK,
    BOXASM_PREFIX_REP,
    BOXASM_PREFIX_REPE,
    BOXASM_PREFIX_REPNE
} BoxAsmPrefix;

typedef struct {
    uint8_t                          Kind;
    uint8_t                          Section;
    uint8_t                          Condition;     /* jcc setcc cmovcc */
    uint8_t                          Prefix;        /* BOXASM_PREFIX_* */
    uint8_t                          OperandCount;
    uint8_t                          Long;          /* a branch grown to rel32; never shrinks */
    uint8_t                          Width;         /* DATA, RESERVE: item size */
    const struct BoxAsmInstruction  *Instruction;
    BoxAsmOperand                    Operands[4];
    uint32_t                         FirstItem;     /* DATA */
    uint32_t                         ItemCount;
    uint32_t                         Count;         /* RESERVE count, ALIGN boundary */
    uint32_t                         Times;         /* `times N` prefix, 0 none */
    uint32_t                         Symbol;        /* LABEL */
    uint64_t                         Offset;
    uint64_t                         Size;
    BoxAsmLocation                   Where;
} BoxAsmStatement;

/* One element of a data statement: an expression, or the bytes of a string. */
typedef struct {
    uint32_t       Expr;
    uint32_t       StringOffset;  /* into BoxAsmAssembler.StringBytes */
    uint32_t       StringLength;
    uint8_t        IsString;
    BoxAsmLocation Where;
} BoxAsmDataItem;

/* ---- Symbols ------------------------------------------------------------ */

typedef enum {
    BOXASM_SYMBOL_UNDEFINED = 0,  /* named, not yet defined */
    BOXASM_SYMBOL_LABEL,
    BOXASM_SYMBOL_CONSTANT,       /* equ */
    BOXASM_SYMBOL_EXTERN,
    BOXASM_SYMBOL_SECTION         /* the start of a section, for $ and $$ */
} BoxAsmSymbolKind;

typedef struct {
    char          *Name;
    uint8_t        Kind;
    uint8_t        Section;
    uint8_t        Global;
    uint8_t        Referenced;     /* reaches the object's relocations */
    uint8_t        Evaluating;     /* an equ in evaluation, to catch cycles */
    uint8_t        Placed;         /* LABEL: given an offset by a layout pass */
    uint64_t       Offset;         /* LABEL: within its section */
    uint32_t       Expr;           /* CONSTANT */
    uint32_t       ElfIndex;
    BoxAsmLocation Defined;
    BoxAsmLocation Used;           /* the first use, for "never defined" */
} BoxAsmSymbol;

/* ---- Output ------------------------------------------------------------- */

typedef enum {
    BOXASM_FIXUP_ABSOLUTE = 1,       /* S + A, unsigned field */
    BOXASM_FIXUP_ABSOLUTE_SIGNED,    /* S + A, sign-extended field */
    BOXASM_FIXUP_RELATIVE,           /* S + A - P, a data reference */
    BOXASM_FIXUP_BRANCH              /* S + A - P, a call or jump */
} BoxAsmFixupKind;

/* A field of an instruction whose value is known only from a symbol. */
typedef struct {
    uint8_t     Kind;
    uint8_t     Offset;      /* of the field within the instruction */
    uint8_t     Width;       /* 1, 4 or 8 */
    uint8_t     TakesAddress;
    uint8_t     Aligned;     /* the boundary the memory it names must sit on, 0 none */
    BoxAsmValue Value;
} BoxAsmFixup;

#define BOXASM_MAX_INSTRUCTION 15u

typedef struct {
    uint8_t     Bytes[BOXASM_MAX_INSTRUCTION];
    uint8_t     Length;
    uint8_t     FixupCount;
    BoxAsmFixup Fixups[2];
} BoxAsmEncoding;

typedef struct {
    uint64_t Offset;
    uint32_t Type;
    uint32_t Symbol;
    int64_t  Addend;
} BoxAsmRelocation;

typedef struct {
    BoxAsmBuffer      Bytes;
    BoxAsmRelocation *Relocations;
    size_t            RelocationCount;
    size_t            RelocationCapacity;
    uint64_t          Size;
    uint64_t          Align;
    uint8_t           Used;
} BoxAsmSectionImage;

/* A code address used as a value: it must begin with endbr64. */
typedef struct {
    uint64_t       Offset;     /* in .text */
    uint32_t       Symbol;     /* the label it names, 0 when it is not one */
    BoxAsmLocation Where;
} BoxAsmTakenAddress;

/* struc ... endstruc, a layout of named offsets written as reservations, and
 * istruc ... iend, one instance of it laid out in data. */
typedef struct {
    uint8_t        Open;           /* between struc and endstruc */
    uint32_t       Name;           /* the struc's symbol */
    uint64_t       Offset;         /* where its next field begins */
    uint32_t       SavedScope;     /* the scope of `.local` names before it */
    BoxAsmLocation Where;
    uint8_t        Instance;       /* between istruc and iend */
    uint32_t       Start;          /* the instance's first byte, a label */
    uint32_t       Size;           /* the instanced struc's NAME_size */
    BoxAsmLocation InstanceWhere;
} BoxAsmStruc;

/* takes and gives: the text of the program's Form so far, from `form 1`. */
typedef struct {
    BoxAsmBuffer Text;
    BoxAsmBuffer Names;          /* the fields' NAMEs so far, each ended by a NUL */
    uint8_t      Takes;
    uint8_t      Into;
    uint8_t      Gives;
} BoxAsmProgramForm;

/* ---- The assembler ------------------------------------------------------ */

typedef struct {
    const char         *FileName;
    char               *Error;
    size_t              ErrorCapacity;

    BoxAsmExpr         *Exprs;
    size_t              ExprCount;
    size_t              ExprCapacity;
    BoxAsmStatement    *Statements;
    size_t              StatementCount;
    size_t              StatementCapacity;
    BoxAsmDataItem     *Items;
    size_t              ItemCount;
    size_t              ItemCapacity;
    BoxAsmBuffer        StringBytes;
    BoxAsmSymbol       *Symbols;
    size_t              SymbolCount;
    size_t              SymbolCapacity;
    BoxAsmNameIndex     SymbolIndex;

    uint8_t             CurrentSection;
    uint8_t             DefaultAbsolute; /* `default abs` */
    uint32_t            Scope;           /* the label `.local` names attach to */
    uint32_t            SayCount;
    uint8_t             Measuring;       /* a layout pass: refusals wait for settled values */
    BoxAsmSectionImage  Sections[BOXASM_SECTION_COUNT];
    BoxAsmTakenAddress *Taken;
    size_t              TakenCount;
    size_t              TakenCapacity;
    const struct BoxAsmExpansion *Expansion; /* the preprocessor's lines and calls */
    uint8_t             Expanded;        /* the text read is Expansion's: lines map back */
    uint32_t            FailFile;        /* while it works: the file of its line */
    uint32_t            FailFrame;       /* while it works: the macro call of its line */
    BoxAsmStruc         Struc;
    BoxAsmProgramForm   Form;
    BoxAsmBuffer        Settings;        /* `setting` lines: the program's settings so far, from `settings 1` */
    BoxAsmBuffer        Life;            /* `life` words: how the program lives, from `life 1` */
    uintptr_t           StackBase;       /* the stack where the work began */
} BoxAsmAssembler;

/* boxasm_source.c */
/* Whether the work has gone too deep into itself to go one level further,
 * for a construct whose nesting began at Start. A source nests macro calls,
 * %defines, parentheses and names defined by names as deep as it likes, each
 * level a frame on the stack, and a BoxOS program's stack grows to
 * BOXOS_USER_STACK_RESERVE with a gap that is never mapped below it: the
 * stack used is what is measured, since what a level costs is the compiler's
 * to say. A construct that itself took little of it is not the one to name:
 * it goes on, and what nests around it refuses at its next level. Start is
 * Asm->StackBase for the outermost. */
int  BoxAsmTooDeep(const BoxAsmAssembler *Asm, uintptr_t Start);
int  BoxAsmFail(BoxAsmAssembler *Asm, const BoxAsmLocation *Where, const char *Text,
                const char *Name);
void BoxAsmFailBegin(BoxAsmAssembler *Asm, const BoxAsmLocation *Where,
                     BoxAsmMessage *Message);
int  BoxAsmFailEnd(BoxAsmAssembler *Asm, const BoxAsmLocation *Where,
                   BoxAsmMessage *Message);
int  BoxAsmGrowArray(void **Items, size_t *Capacity, size_t Count, size_t Size);
uint32_t BoxAsmSectionSymbol(BoxAsmSection Section);
extern const char *const BoxAsmSectionNames[BOXASM_SECTION_COUNT];

/* boxasm_expr.c */
int BoxAsmSymbolClaim(BoxAsmAssembler *Asm, const char *Name, size_t Length,
                      const BoxAsmLocation *Where, uint32_t *Out);
int BoxAsmParseExpression(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer,
                          BoxAsmToken *Token, uint32_t *Out);
/* One term of a sum: what * / % and unary operators bind, nothing looser.
 * A memory operand adds its terms itself, registers among them. */
int BoxAsmParseTerm(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer,
                    BoxAsmToken *Token, uint32_t *Out);
int BoxAsmMakeExpr(BoxAsmAssembler *Asm, const BoxAsmExpr *Node, uint32_t *Out);
/* The full name of a label as written: `.x` belongs to the last plain label. */
int BoxAsmLabelName(BoxAsmAssembler *Asm, const BoxAsmToken *Token, char **Out);
int BoxAsmEvaluate(BoxAsmAssembler *Asm, uint32_t Expr, uint64_t Here,
                   uint8_t Section, BoxAsmValue *Out);
int BoxAsmEvaluateConstant(BoxAsmAssembler *Asm, uint32_t Expr, uint64_t Here,
                           uint8_t Section, int64_t *Out);
int BoxAsmNextToken(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token);

/* boxasm_operand.c */
int BoxAsmRegisterLookup(const char *Text, size_t Length, BoxAsmRegister *Out);
int BoxAsmParseOperand(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                       BoxAsmOperand *Out);
int BoxAsmRegisterOperand(const char *Name, BoxAsmOperand *Out);

/* boxasm_parse.c */
int BoxAsmParse(BoxAsmAssembler *Asm, const char *Source, size_t Size);
int BoxAsmAddStatement(BoxAsmAssembler *Asm, uint8_t Kind, const BoxAsmLocation *Where,
                       uint32_t *Out);
int BoxAsmAddInstruction(BoxAsmAssembler *Asm, const BoxAsmLocation *Where, const char *Name,
                         const BoxAsmOperand *Operands, uint8_t Count);
int BoxAsmDefineLabel(BoxAsmAssembler *Asm, const char *Name, const BoxAsmLocation *Where,
                      uint32_t *Out);
int BoxAsmAddBytes(BoxAsmAssembler *Asm, const BoxAsmLocation *Where, const uint8_t *Bytes,
                   size_t Length);

/* boxasm_box.c */
int BoxAsmParseBox(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                   const BoxAsmToken *Word);

/* boxasm_form.c */
int BoxAsmParseForm(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                    const BoxAsmToken *Word);

/* boxasm_settings.c */
int BoxAsmParseProgramSetting(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                              const BoxAsmToken *Word);

/* boxasm_life.c */
int BoxAsmParseProgramLife(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                           const BoxAsmToken *Word);

/* boxasm_struc.c */
int BoxAsmStrucOpen(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                    const BoxAsmToken *Word);
int BoxAsmStrucClose(BoxAsmAssembler *Asm, const BoxAsmToken *Word);
int BoxAsmStrucField(BoxAsmAssembler *Asm, const char *Name, const BoxAsmLocation *Where);
/* resb, resw, resd, resq N in a struc (Width bytes each), or align N (Width 0). */
int BoxAsmStrucReserve(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                       const BoxAsmToken *Word, uint8_t Width);
int BoxAsmInstanceOpen(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                       const BoxAsmToken *Word);
int BoxAsmInstancePad(BoxAsmAssembler *Asm, const BoxAsmLocation *Where, uint32_t Field);
int BoxAsmInstanceClose(BoxAsmAssembler *Asm, const BoxAsmToken *Word);
int BoxAsmStrucCheckClosed(BoxAsmAssembler *Asm);

/* boxasm_layout.c */
int BoxAsmLayout(BoxAsmAssembler *Asm);
int BoxAsmEmit(BoxAsmAssembler *Asm);

/* boxasm_object.c */
int BoxAsmWriteObject(BoxAsmAssembler *Asm, BoxAsmBlob *Out);

#endif
