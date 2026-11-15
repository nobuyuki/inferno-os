#include "lib9.h"
#include "isa.h"
#include "interp.h"
#include "raise.h"

/*
 * Just-in-time compiler from Dis to amd64 (System V calling convention).
 *
 * A Dis word is 64 bits here, but Limbo's int is a 32-bit value kept
 * sign-extended in its word (see include/interp.h).  Words are therefore
 * moved and compared as 64-bit quantities (they might hold addresses),
 * but int arithmetic is done in 32 bits and the result sign-extended.
 *
 * Registers:
 *	RFP, RMP	Dis frame and module pointers (callee-saved by C)
 *	RREG		address of the interpreter's register set, R
 *	RSAVE		holds the stack pointer across calls to C
 *	RAX, RCX, RTA (RDX), RSI, RDI	temporaries
 *
 * Compiled code is entered at any Dis instruction boundary by a jump
 * through comvec, which is called like a C function by xec, and returns
 * to xec to reschedule or to run interpreted code.  The stack holds
 * only the registers saved by comvec, and return addresses of calls to
 * the macros below (never more than two deep).
 */

#define	RESCHED 1	/* check for interpreter reschedule */

enum
{
	RAX	= 0,
	RCX	= 1,
	RDX	= 2,
	RBX	= 3,
	RSP	= 4,
	RBP	= 5,
	RSI	= 6,
	RDI	= 7,
	R8	= 8,
	R9	= 9,
	R10	= 10,
	R11	= 11,
	R12	= 12,
	R13	= 13,
	R14	= 14,
	R15	= 15,

	RFP	= RBX,
	RMP	= R13,
	RREG	= R15,
	RSAVE	= R12,
	RTA	= RDX,

	/* flags qualifying an opcode byte, for the instruction generators */
	W	= 1<<16,	/* 64-bit operand size (REX.W) */
	ESC	= 1<<17,	/* two-byte opcode: 0F xx */
	P66	= 1<<18,	/* operand size/SSE prefix 66 */
	PF2	= 1<<19,	/* SSE prefix F2 */
	B8	= 1<<20,	/* the register operand is a byte register */

	Oldq	= W|0x8b,	/* MOV r64, r/m64 */
	Oldl	= 0x8b,		/* MOV r32, r/m32 (zero-extends) */
	Oldsx	= W|0x63,	/* MOVSXD r64, r/m32 */
	Oldb	= B8|0x8a,	/* MOV r8, r/m8 */
	Oldzb	= ESC|0xb6,	/* MOVZX r32, r/m8 */
	Ostq	= W|0x89,	/* MOV r/m64, r64 */
	Ostl	= 0x89,		/* MOV r/m32, r32 */
	Ostb	= B8|0x88,	/* MOV r/m8, r8 */
	Olea	= W|0x8d,	/* LEA r64, m */
	Omoviq	= W|0xc7,	/* MOV r/m64, imm32 (sign-extended); /0 */
	Omovil	= 0xc7,		/* MOV r/m32, imm32; /0 */
	Ocmpq	= W|0x39,	/* CMP r/m64, r64 */
	Ocmpb	= B8|0x38,	/* CMP r/m8, r8 */
	Oimul	= ESC|0xaf,	/* IMUL r, r/m */
	Oldf	= PF2|ESC|0x10,	/* MOVSD xmm, m64 */
	Ostf	= PF2|ESC|0x11,	/* MOVSD m64, xmm */
	Oaddf	= PF2|ESC|0x58,
	Omulf	= PF2|ESC|0x59,
	Osubf	= PF2|ESC|0x5c,
	Odivf	= PF2|ESC|0x5e,
	Ocvtlf	= PF2|ESC|0x2a,	/* CVTSI2SD xmm, r/m32 (r/m64 with W) */
	Oucomf	= P66|ESC|0x2e,	/* UCOMISD xmm, m64 */
	Ocomf	= P66|ESC|0x2f,	/* COMISD xmm, m64 */

	Ocall	= 0xe8,
	Ojmp	= 0xe9,
	Ojmpb	= 0xeb,
	Oret	= 0xc3,
	Opush	= 0x50,
	Opop	= 0x58,
	Ocld	= 0xfc,

	/* condition codes */
	CCB	= 0x2,
	CCAE	= 0x3,
	CCE	= 0x4,
	CCNE	= 0x5,
	CCBE	= 0x6,
	CCA	= 0x7,
	CCP	= 0xa,
	CCL	= 0xc,
	CCGE	= 0xd,
	CCLE	= 0xe,
	CCG	= 0xf,

	SRCOP	= (1<<0),
	DSTOP	= (1<<1),
	WRTPC	= (1<<2),
	TCHECK	= (1<<3),
	NEWPC	= (1<<4),
	DBRAN	= (1<<5),
	THREOP	= (1<<6),

	MacFRP	= 0,
	MacRET	= 1,
	MacCOLR	= 2,
	MacMCAL	= 3,
	MacFRAM	= 4,
	MacMFRA	= 5,
	MacRELQ = 6,
	MacCASE = 7,
	NMACRO
};

static	uchar*	code;
static	uchar*	base;
static	ulong*	patch;
static	int	pass;
static	Module*	mod;
static	uchar*	tinit;
static	WORD*	litpool;
static	int	nlit;
static	void	macfrp(void);
static	void	macret(void);
static	void	maccolr(void);
static	void	macmcal(void);
static	void	macfram(void);
static	void	macmfra(void);
static	void	macrelq(void);
static	void	maccase(void);
static	ulong	macro[NMACRO];
	void	(*comvec)(void);
extern	void	das(uchar*, int);

#define T(r)	*((void**)(R.r))
#define	HREF	(O(Heap, ref)-(long)sizeof(Heap))
#define	HCOLOR	(O(Heap, color)-(long)sizeof(Heap))

struct
{
	int	idx;
	void	(*gen)(void);
} mactab[] =
{
	MacFRP,		macfrp,		/* decrement and free pointer */
	MacRET,		macret,		/* return instruction */
	MacCOLR,	maccolr,	/* increment and color pointer */
	MacMCAL,	macmcal,	/* mcall bottom half */
	MacFRAM,	macfram,	/* frame instruction */
	MacMFRA,	macmfra,	/* punt mframe because t->initialize==0 */
	MacRELQ,	macrelq,	/* reschedule */
	MacCASE,	maccase,	/* case instruction */
};

static void
bounds(void)
{
	error(exBounds);
}

static void
rdestroy(void)
{
	destroy(R.s);
}

static void
rmcall(void)
{
	Prog *p;
	Frame *f;

	if((void*)R.dt == H)
		error(exModule);

	f = (Frame*)R.FP;
	if(f == H)
		error(exModule);

	f->mr = nil;
	((void(*)(Frame*))R.dt)(f);
	R.SP = (uchar*)f;
	R.FP = f->fp;
	if(f->t == nil)
		unextend(f);
	else
		freeptrs(f, f->t);
	p = currun();
	if(p->kill != nil)
		error(p->kill);
}

static void
rmfram(void)
{
	Type *t;
	Frame *f;
	uchar *nsp;

	t = (Type*)R.s;
	if(t == H)
		error(exModule);

	nsp = R.SP + t->size;
	if(nsp >= R.TS) {
		R.s = t;
		extend();
		T(d) = R.s;
		return;
	}
	f = (Frame*)R.SP;
	R.SP = nsp;
	f->t = t;
	f->mr = nil;
	initmem(t, f);
	T(d) = f;
}

static int
bc(vlong o)
{
	return o >= -128 && o <= 127;
}

static void
urk(void)
{
	error(exCompile);
}

static void
genb(uchar o)
{
	*code++ = o;
}

static void
gen2(uchar o1, uchar o2)
{
	code[0] = o1;
	code[1] = o2;
	code += 2;
}

static void
genw(ulong o)
{
	code[0] = o;
	code[1] = o>>8;
	code[2] = o>>16;
	code[3] = o>>24;
	code += 4;
}

static void
genq(uvlong o)
{
	genw(o);
	genw(o>>32);
}

/*
 * prefixes and opcode: reg, index and base are the registers
 * that will appear in the modrm and sib bytes that follow
 */
static void
opcode(int op, int reg, int index, int base)
{
	int rex;

	if(op & P66)
		genb(0x66);
	if(op & PF2)
		genb(0xf2);
	rex = 0x40;
	if(op & W)
		rex |= 8;
	if(reg & 8)
		rex |= 4;
	if(index & 8)
		rex |= 2;
	if(base & 8)
		rex |= 1;
	if(rex != 0x40 || (op & B8) && reg >= RSP)
		genb(rex);
	if(op & ESC)
		genb(0x0f);
	genb(op);
}

/* op reg, disp(base) */
static void
modrm(int op, long disp, int base, int reg)
{
	int m;

	opcode(op, reg, 0, base);
	reg &= 7;
	if(disp == 0 && (base&7) != RBP)
		m = 0;
	else if(bc(disp))
		m = 1;
	else
		m = 2;
	genb((m<<6)|(reg<<3)|(base&7));
	if((base&7) == RSP)
		genb(0x24);
	if(m == 1)
		genb(disp);
	else if(m == 2)
		genw(disp);
}

/* op reg, disp(base)(index*(1<<scale)) */
static void
modsib(int op, long disp, int base, int index, int scale, int reg)
{
	int m;

	opcode(op, reg, index, base);
	reg &= 7;
	if(disp == 0 && (base&7) != RBP)
		m = 0;
	else if(bc(disp))
		m = 1;
	else
		m = 2;
	genb((m<<6)|(reg<<3)|4);
	genb((scale<<6)|((index&7)<<3)|(base&7));
	if(m == 1)
		genb(disp);
	else if(m == 2)
		genw(disp);
}

/* op reg, rm (both registers) */
static void
modrr(int op, int rm, int reg)
{
	opcode(op, reg, 0, rm);
	genb((3<<6)|((reg&7)<<3)|(rm&7));
}

/* load the sign-extended constant o into r */
static void
con(long o, int r)
{
	if(o == 0) {
		modrr(0x31, r, r);		/* XORL r, r */
		return;
	}
	modrr(Omoviq, r, 0);
	genw(o);
}

/* load a 64-bit constant (an address) into r */
static void
conp(void *p, int r)
{
	opcode(W|(0xb8+(r&7)), 0, 0, r);
	genq((uintptr)p);
}

/* offset from base of the code now being generated */
static ulong
dot(void)
{
	return code-base;
}

/* LEA of the code at offset dst from base, relative to the instruction pointer */
static void
leacode(ulong dst, int r)
{
	opcode(Olea, r, 0, 0);
	genb((0<<6)|((r&7)<<3)|5);
	genw(dst - (dot()+4));
}

/* LEA of an address near the code (the literal pool), likewise */
static void
leanear(void *a, int r)
{
	opcode(Olea, r, 0, 0);
	genb((0<<6)|((r&7)<<3)|5);
	genw((uchar*)a - (code+4));
}

/* branch (jump, call or conditional jump) to the code at offset dst */
static void
rbra(ulong dst, int op)
{
	genb(op);
	genw(dst - (dot()+4));
}

static void
jcc(ulong dst, int cc)
{
	genb(0x0f);
	rbra(dst, 0x80|cc);
}

/* short forward branches, to be completed by label() */
static uchar*
jccb(int cc)
{
	gen2(0x70|cc, 0);
	return code-1;
}

static uchar*
jmpb(void)
{
	gen2(Ojmpb, 0);
	return code-1;
}

static void
label(uchar *l)
{
	if(!bc(code-l-1))
		urk();
	*l = code-l-1;
}

/* CMPQ r, $v */
static void
cmpi(int r, long v)
{
	if(bc(v)) {
		modrr(W|0x83, r, 7);
		genb(v);
		return;
	}
	modrr(W|0x81, r, 7);
	genw(v);
}

/* CMPQ r, $H */
static void
cmph(int r)
{
	cmpi(r, -1);
}

/*
 * call a C function: any caller-saved register can be lost.
 * The stack might not be aligned as C requires, because
 * the macros are called at different depths.
 */
static void
ccall(void *fn)
{
	modrr(Ostq, RSAVE, RSP);		/* MOVQ SP, RSAVE */
	modrr(W|0x83, RSP, 4);			/* ANDQ $~15, SP */
	genb(0xf0);
	conp(fn, RAX);
	gen2(0xff, (3<<6)|(2<<3)|RAX);		/* CALL* AX */
	modrr(Ostq, RSP, RSAVE);		/* MOVQ RSAVE, SP */
}

/* raise a bounds error in instruction i, recording its pc for the handler */
static void
cbounds(Inst *i)
{
	leacode(patch[i-mod->prog+1], RAX);
	modrm(Ostq, O(REG, PC), RREG, RAX);
	modrm(Ostq, O(REG, FP), RREG, RFP);
	ccall(bounds);
}

/* leave compiled code: return from comvec to xec */
static void
leave(void)
{
	opcode(Opop+(R15&7), 0, 0, R15);
	opcode(Opop+(R14&7), 0, 0, R14);
	opcode(Opop+(R13&7), 0, 0, R13);
	opcode(Opop+(R12&7), 0, 0, R12);
	genb(Opop+RBX);
	genb(Oret);
}

/*
 * apply instruction mi with register r to the source operand.
 * an immediate source is simply loaded into r, whatever mi is.
 */
static void
opwld(Inst *i, int mi, int r)
{
	int ir, rta;

	switch(UXSRC(i->add)) {
	default:
		print("%D\n", i);
		urk();
	case SRC(AFP):
		modrm(mi, i->s.ind, RFP, r);
		return;
	case SRC(AMP):
		modrm(mi, i->s.ind, RMP, r);
		return;
	case SRC(AIMM):
		con(i->s.imm, r);
		return;
	case SRC(AIND|AFP):
		ir = RFP;
		break;
	case SRC(AIND|AMP):
		ir = RMP;
		break;
	}
	rta = RTA;
	if(mi == Olea)
		rta = r;
	modrm(Oldq, i->s.i.f, ir, rta);
	modrm(mi, i->s.i.s, rta, r);
}

/* likewise the destination operand */
static void
opwst(Inst *i, int mi, int r)
{
	int ir, rta;

	switch(UXDST(i->add)) {
	default:
		print("%D\n", i);
		urk();
	case DST(AIMM):
		con(i->d.imm, r);
		return;
	case DST(AFP):
		modrm(mi, i->d.ind, RFP, r);
		return;
	case DST(AMP):
		modrm(mi, i->d.ind, RMP, r);
		return;
	case DST(AIND|AFP):
		ir = RFP;
		break;
	case DST(AIND|AMP):
		ir = RMP;
		break;
	}
	rta = RTA;
	if(mi == Olea)
		rta = r;
	modrm(Oldq, i->d.i.f, ir, rta);
	modrm(mi, i->d.i.s, rta, r);
}

/* likewise the middle operand, which is the destination if there is none */
static void
mid(Inst *i, int mi, int r)
{
	int ir;

	switch(i->add&ARM) {
	default:
		opwst(i, mi, r);
		return;
	case AXIMM:
		con((short)i->reg, r);
		return;
	case AXINF:
		ir = RFP;
		break;
	case AXINM:
		ir = RMP;
		break;
	}
	modrm(mi, i->reg, ir, r);
}

/* store a 64-bit register in the middle operand, which must be in memory */
static void
midst(Inst *i, int r)
{
	int ir;

	ir = RMP;
	if((i->add&ARM) == AXINF)
		ir = RFP;
	modrm(Ostq, i->reg, ir, r);
}

/*
 * put the word imm in the literal pool, and its address in R at roff
 */
static void
literal(WORD imm, int roff)
{
	leanear(litpool+nlit, RAX);
	modrm(Ostq, roff, RREG, RAX);
	if(pass)
		litpool[nlit] = imm;
	nlit++;
}

static void
punt(Inst *i, int m, void (*fn)(void))
{
	ulong pc;
	uchar *l;

	if(m & SRCOP) {
		if(UXSRC(i->add) == SRC(AIMM))
			literal(i->s.imm, O(REG, s));
		else {
			opwld(i, Olea, RAX);
			modrm(Ostq, O(REG, s), RREG, RAX);
		}
	}

	if(m & DSTOP) {
		if(UXDST(i->add) == DST(AIMM))
			literal(i->d.imm, O(REG, d));
		else {
			opwst(i, Olea, RAX);
			modrm(Ostq, O(REG, d), RREG, RAX);
		}
	}
	/*
	 * always record the pc, so that an exception raised by fn finds the
	 * right handler (it is the next instruction's, as when interpreting)
	 */
	if(i >= mod->prog && i < mod->prog+mod->nprog) {
		leacode(patch[i-mod->prog+1], RAX);
		modrm(Ostq, O(REG, PC), RREG, RAX);
	}
	if(m & DBRAN) {
		pc = patch[(Inst*)i->d.imm-mod->prog];
		literal((WORD)(base+pc), O(REG, d));
	}

	switch(i->add&ARM) {
	case AXNON:
		if(m & THREOP) {
			modrm(Oldq, O(REG, d), RREG, RAX);
			modrm(Ostq, O(REG, m), RREG, RAX);
		}
		break;
	case AXIMM:
		literal((short)i->reg, O(REG, m));
		break;
	case AXINF:
		modrm(Olea, i->reg, RFP, RAX);
		modrm(Ostq, O(REG, m), RREG, RAX);
		break;
	case AXINM:
		modrm(Olea, i->reg, RMP, RAX);
		modrm(Ostq, O(REG, m), RREG, RAX);
		break;
	}
	modrm(Ostq, O(REG, FP), RREG, RFP);

	ccall(fn);

	if(m & TCHECK) {
		modrm(W|0x83, O(REG, t), RREG, 7);	/* CMPQ R.t, $0 */
		genb(0x00);
		l = jccb(CCE);
		leave();
		label(l);
	}

	modrm(Oldq, O(REG, FP), RREG, RFP);
	modrm(Oldq, O(REG, MP), RREG, RMP);

	if(m & NEWPC) {
		modrm(Oldq, O(REG, PC), RREG, RAX);
		gen2(0xff, (3<<6)|(4<<3)|RAX);		/* JMP* AX */
	}
}

/*
 * int arithmetic: d = m op s in 32 bits, sign-extended.
 * op is the form `op r32, r/m32'; rm the /digit of the immediate form.
 */
static void
arith(Inst *i, int op, int rm)
{
	mid(i, Oldl, RAX);
	if(UXSRC(i->add) == SRC(AIMM)) {
		if(bc(i->s.imm)) {
			modrr(0x83, RAX, rm);
			genb(i->s.imm);
		}
		else {
			modrr(0x81, RAX, rm);
			genw(i->s.imm);
		}
	}
	else
		opwld(i, op, RAX);
	modrr(Oldsx, RAX, RAX);
	opwst(i, Ostq, RAX);
}

/* big arithmetic: d = m op s in 64 bits */
static void
arithl(Inst *i, int op)
{
	mid(i, Oldq, RAX);
	if(UXSRC(i->add) == SRC(AIMM)) {
		con(i->s.imm, RCX);
		modrr(W|op, RCX, RAX);
	}
	else
		opwld(i, W|op, RAX);
	opwst(i, Ostq, RAX);
}

/* byte arithmetic */
static void
arithb(Inst *i, int op)
{
	mid(i, Oldzb, RAX);
	if(UXSRC(i->add) == SRC(AIMM)) {
		con(i->s.imm, RCX);
		modrr(op+1, RCX, RAX);		/* the 32-bit form will do */
	}
	else
		opwld(i, B8|op, RAX);
	opwst(i, Ostb, RAX);
}

/* d = m shift s; ld loads the value, st stores it, w qualifies the shift */
static void
shift(Inst *i, int ld, int st, int w, int r)
{
	mid(i, ld, RAX);
	opwld(i, Oldl, RCX);
	modrr(w|0xd3, RAX, r);
	if(st == Ostq && w == 0)
		modrr(Oldsx, RAX, RAX);
	opwst(i, st, RAX);
}

static void
arithf(Inst *i, int op)
{
	mid(i, Oldf, 0);
	opwld(i, op, 0);
	opwst(i, Ostf, 0);
}

static int
swapcc(int cc)
{
	switch(cc) {
	case CCGE:
		return CCLE;
	case CCLE:
		return CCGE;
	case CCG:
		return CCL;
	case CCL:
		return CCG;
	case CCAE:
		return CCBE;
	case CCBE:
		return CCAE;
	case CCA:
		return CCB;
	case CCB:
		return CCA;
	}
	return cc;
}

static void
schedcheck(Inst *i)
{
	uchar *l;

	if(RESCHED && i->d.ins <= i){
		modrm(0x83, O(REG, IC), RREG, 5);	/* SUBL $1, R.IC */
		genb(1);
		l = jccb(CCG);
		rbra(macro[MacRELQ], Ocall);
		label(l);
	}
}

/* branch if s cc m, for words (which might hold pointers) and bigs */
static void
cbra(Inst *i, int cc)
{
	if(RESCHED)
		schedcheck(i);
	mid(i, Oldq, RAX);
	if(UXSRC(i->add) == SRC(AIMM)) {
		cmpi(RAX, i->s.imm);
		cc = swapcc(cc);
	}
	else
		opwld(i, Ocmpq, RAX);
	jcc(patch[i->d.ins-mod->prog], cc);
}

static void
cbrab(Inst *i, int cc)
{
	if(RESCHED)
		schedcheck(i);
	mid(i, Oldb, RAX);
	if(UXSRC(i->add) == SRC(AIMM)) {
		con(i->s.imm, RCX);
		modrr(Ocmpb, RCX, RAX);			/* CMPB CL, AL */
	}
	else
		opwld(i, Ocmpb, RAX);
	jcc(patch[i->d.ins-mod->prog], cc);
}

/*
 * branch on a comparison of reals: the operands are arranged
 * so that the result is false if they are unordered, except for !=.
 */
static void
cbraf(Inst *i, int op)
{
	ulong dst;
	uchar *l;

	if(RESCHED)
		schedcheck(i);
	dst = patch[i->d.ins-mod->prog];
	switch(op) {
	case IBLTF:	/* s < m, ie m > s */
	case IBLEF:
		mid(i, Oldf, 0);
		opwld(i, Ocomf, 0);
		jcc(dst, op == IBLTF? CCA: CCAE);
		break;
	case IBGTF:
	case IBGEF:
		opwld(i, Oldf, 0);
		mid(i, Ocomf, 0);
		jcc(dst, op == IBGTF? CCA: CCAE);
		break;
	case IBEQF:
		opwld(i, Oldf, 0);
		mid(i, Oucomf, 0);
		l = jccb(CCP);
		jcc(dst, CCE);
		label(l);
		break;
	case IBNEF:
		opwld(i, Oldf, 0);
		mid(i, Oucomf, 0);
		jcc(dst, CCP);
		jcc(dst, CCNE);
		break;
	}
}

/*
 * replace the Dis pcs in a case table by addresses of compiled code
 */
static void
comcase(Inst *i)
{
	int l;
	WORD *t, *e;

	t = (WORD*)(mod->origmp+i->d.ind+IBY2WD);
	l = t[-1];

	/* have to take care not to relocate the same table twice -
	 * the limbo compiler can duplicate a case instruction
	 * during its folding phase
	 */

	if(pass == 0) {
		if(l >= 0)
			t[-1] = -l-1;	/* Mark it not done */
		return;
	}
	if(l >= 0)			/* Check pass 2 done */
		return;
	t[-1] = -l-1;			/* Set real count */
	e = t + t[-1]*3;
	while(t < e) {
		t[2] = (WORD)(base + patch[t[2]]);
		t += 3;
	}
	t[0] = (WORD)(base + patch[t[0]]);
}

static void
comcasel(Inst *i)
{
	int l;
	WORD *t, *e;
	enum { Pcoff = 2*IBY2LG/IBY2WD, Stride = Pcoff+2 };	/* see casel in xec.c */

	t = (WORD*)(mod->origmp+i->d.ind+2*IBY2WD);
	l = t[-2];
	if(pass == 0) {
		if(l >= 0)
			t[-2] = -l-1;	/* Mark it not done */
		return;
	}
	if(l >= 0)			/* Check pass 2 done */
		return;
	t[-2] = -l-1;			/* Set real count */
	e = t + t[-2]*Stride;
	while(t < e) {
		t[Pcoff] = (WORD)(base + patch[t[Pcoff]]);
		t += Stride;
	}
	t[0] = (WORD)(base + patch[t[0]]);
}

static void
commframe(Inst *i)
{
	int o;
	uchar *punt, *mlnil;

	opwld(i, Oldq, RAX);
	cmph(RAX);
	mlnil = jccb(CCE);
	o = OA(Modlink, links)+i->reg*sizeof(Modl)+O(Modl, frame);
	modrm(Oldq, o, RAX, RTA);
	modrm(W|0x83, O(Type, initialize), RTA, 7);	/* CMPQ initialize(RTA), $0 */
	genb(0);
	punt = jccb(CCNE);
	modrr(Ostq, RAX, RTA);				/* MOVQ RTA, AX: the type */
	opwst(i, Olea, RTA);
	label(mlnil);
	rbra(macro[MacMFRA], Ocall);
	rbra(patch[i-mod->prog+1], Ojmp);

	label(punt);
	rbra(macro[MacFRAM], Ocall);
	opwst(i, Ostq, RCX);
}

static void
commcall(Inst *i)
{
	uchar *mlnil;

	opwld(i, Oldq, RCX);
	leacode(patch[i-mod->prog+1], RAX);
	modrm(Ostq, O(Frame, lr), RCX, RAX);	/* f->lr = R.PC */
	modrm(Ostq, O(Frame, fp), RCX, RFP); 	/* f->fp = R.FP */
	modrm(Oldq, O(REG, M), RREG, RTA);
	modrm(Ostq, O(Frame, mr), RCX, RTA);	/* f->mr = R.M */
	opwst(i, Oldq, RTA);			/* ml */
	cmph(RTA);
	mlnil = jccb(CCE);
	modrm(Oldq, OA(Modlink, links)+i->reg*sizeof(Modl)+O(Modl, u.pc), RTA, RAX);
	label(mlnil);
	rbra(macro[MacMCAL], Ocall);
}

static void
comp(Inst *i)
{
	int r;
	WORD *t, *e;
	uchar *l, *l1, *loop;
	char buf[64];

	switch(i->op) {
	default:
		snprint(buf, sizeof buf, "%s compile, no '%D'", mod->name, i);
		error(buf);
		break;
	case IMCALL:
		if((i->add&ARM) == AXIMM)
			commcall(i);
		else
			punt(i, SRCOP|DSTOP|THREOP|WRTPC|TCHECK|NEWPC, optab[i->op]);
		break;
	case ISEND:
	case IRECV:
	case IALT:
		punt(i, SRCOP|DSTOP|TCHECK|WRTPC, optab[i->op]);
		break;
	case ISPAWN:
		punt(i, SRCOP|DBRAN, optab[i->op]);
		break;
	case IBNEC:
	case IBEQC:
	case IBLTC:
	case IBLEC:
	case IBGTC:
	case IBGEC:
		punt(i, SRCOP|DBRAN|NEWPC|WRTPC, optab[i->op]);
		break;
	case ICASE:
		comcase(i);
		opwld(i, Oldq, RAX);			/* value */
		opwst(i, Olea, RSI);			/* table */
		rbra(macro[MacCASE], Ojmp);
		break;
	case ICASEC:
		comcase(i);
		punt(i, SRCOP|DSTOP|NEWPC, optab[i->op]);
		break;
	case ICASEL:
		comcasel(i);
		punt(i, SRCOP|DSTOP|NEWPC, optab[i->op]);
		break;
	case IADDC:
	case IMNEWZ:
	case IMULB:
	case IDIVB:
	case IMODB:
		punt(i, SRCOP|DSTOP|THREOP, optab[i->op]);
		break;
	case ILOAD:
	case INEWA:
	case INEWAZ:
	case INEW:
	case INEWZ:
	case ISLICEA:
	case ISLICELA:
	case ICONSB:
	case ICONSW:
	case ICONSL:
	case ICONSF:
	case ICONSM:
	case ICONSMP:
	case ICONSP:
	case IMOVMP:
	case IHEADMP:
	case IINSC:
	case ICVTAC:
	case ICVTCW:
	case ICVTWC:
	case ICVTLC:
	case ICVTCL:
	case ICVTFC:
	case ICVTCF:
	case ICVTRF:
	case ICVTFR:
	case ICVTWS:
	case ICVTSW:
	case ICVTFW:
	case ICVTFL:
	case IMSPAWN:
	case ICVTCA:
	case ISLICEC:
	case INBALT:
		punt(i, SRCOP|DSTOP, optab[i->op]);
		break;
	case INEWCM:
	case INEWCMP:
		punt(i, SRCOP|DSTOP|THREOP, optab[i->op]);
		break;
	case IMFRAME:
		if((i->add&ARM) == AXIMM)
			commframe(i);
		else
			punt(i, SRCOP|DSTOP|THREOP, optab[i->op]);
		break;
	case INEWCB:
	case INEWCW:
	case INEWCF:
	case INEWCP:
	case INEWCL:
		punt(i, DSTOP|THREOP, optab[i->op]);
		break;
	case IEXIT:
		punt(i, 0, optab[i->op]);
		break;
	case ICVTBW:
		opwld(i, Oldzb, RAX);
		opwst(i, Ostq, RAX);
		break;
	case ICVTWB:
		opwld(i, Oldl, RAX);
		opwst(i, Ostb, RAX);
		break;
	case ICVTWF:
		if(UXSRC(i->add) == SRC(AIMM)) {
			con(i->s.imm, RAX);
			modrr(Ocvtlf, RAX, 0);
		}
		else
			opwld(i, Ocvtlf, 0);
		opwst(i, Ostf, 0);
		break;
	case ICVTLF:
		opwld(i, W|Ocvtlf, 0);
		opwst(i, Ostf, 0);
		break;
	case IHEADM:
		opwld(i, Oldq, RAX);
		modrm(Olea, OA(List, data), RAX, RSI);
		goto movm;
	case IMOVM:
		opwld(i, Olea, RSI);
	movm:
		opwst(i, Olea, RDI);
		mid(i, Oldl, RCX);
		genb(Ocld);
		gen2(0xf3, 0xa4);			/* REP MOVSB */
		break;
	case IRET:
		rbra(macro[MacRET], Ojmp);
		break;
	case IFRAME:
		if(UXSRC(i->add) != SRC(AIMM)) {
			punt(i, SRCOP|DSTOP, optab[i->op]);
			break;
		}
		tinit[i->s.imm] = 1;
		conp(mod->type[i->s.imm], RTA);
		rbra(macro[MacFRAM], Ocall);
		opwst(i, Ostq, RCX);
		break;
	case ILEA:
		if(UXSRC(i->add) == SRC(AIMM)) {
			gen2(Ojmpb, 8);
			genq(i->s.imm);
			leanear(code-8, RAX);
		}
		else
			opwld(i, Olea, RAX);
		opwst(i, Ostq, RAX);
		break;
	case IHEADW:
	case IHEADL:
	case IHEADF:
		opwld(i, Oldq, RAX);
		modrm(Oldq, OA(List, data), RAX, RAX);
		opwst(i, Ostq, RAX);
		break;
	case IHEADB:
		opwld(i, Oldq, RAX);
		modrm(Oldb, OA(List, data), RAX, RAX);
		opwst(i, Ostb, RAX);
		break;
	case ITAIL:
		opwld(i, Oldq, RAX);
		modrm(Oldq, O(List, tail), RAX, RSI);
		goto movp;
	case IMOVP:
	case IHEADP:
		opwld(i, Oldq, RSI);
		if(i->op == IHEADP)
			modrm(Oldq, OA(List, data), RSI, RSI);
	movp:
		cmph(RSI);
		l = jccb(CCE);
		rbra(macro[MacCOLR], Ocall);
		label(l);
		opwst(i, Oldq, RAX);
		opwst(i, Ostq, RSI);
		rbra(macro[MacFRP], Ocall);
		break;
	case ILENA:
		opwld(i, Oldq, RSI);
		con(0, RAX);
		cmph(RSI);
		l = jccb(CCE);
		modrm(Oldq, O(Array, len), RSI, RAX);
		label(l);
		opwst(i, Ostq, RAX);
		break;
	case ILENC:
		opwld(i, Oldq, RSI);
		con(0, RAX);
		cmph(RSI);
		l = jccb(CCE);
		modrm(Oldsx, O(String, len), RSI, RAX);
		cmpi(RAX, 0);
		l1 = jccb(CCGE);
		modrr(W|0xf7, RAX, 3);			/* NEGQ AX */
		label(l1);
		label(l);
		opwst(i, Ostq, RAX);
		break;
	case ILENL:
		con(0, RAX);
		opwld(i, Oldq, RSI);
		loop = code;
		cmph(RSI);
		l = jccb(CCE);
		modrm(Oldq, O(List, tail), RSI, RSI);
		modrr(W|0xff, RAX, 0);			/* INCQ AX */
		gen2(Ojmpb, loop-code-2);
		label(l);
		opwst(i, Ostq, RAX);
		break;
	case IBEQF:
	case IBNEF:
	case IBLEF:
	case IBLTF:
	case IBGEF:
	case IBGTF:
		cbraf(i, i->op);
		break;
	case IBEQW:
	case IBEQL:
		cbra(i, CCE);
		break;
	case IBLEW:
	case IBLEL:
		cbra(i, CCLE);
		break;
	case IBNEW:
	case IBNEL:
		cbra(i, CCNE);
		break;
	case IBGTW:
	case IBGTL:
		cbra(i, CCG);
		break;
	case IBLTW:
	case IBLTL:
		cbra(i, CCL);
		break;
	case IBGEW:
	case IBGEL:
		cbra(i, CCGE);
		break;
	case IBEQB:
		cbrab(i, CCE);
		break;
	case IBLEB:
		cbrab(i, CCBE);
		break;
	case IBNEB:
		cbrab(i, CCNE);
		break;
	case IBGTB:
		cbrab(i, CCA);
		break;
	case IBLTB:
		cbrab(i, CCB);
		break;
	case IBGEB:
		cbrab(i, CCAE);
		break;
	case ISUBW:
		arith(i, 0x2b, 5);
		break;
	case ISUBL:
		arithl(i, 0x2b);
		break;
	case ISUBB:
		arithb(i, 0x2a);
		break;
	case ISUBF:
		arithf(i, Osubf);
		break;
	case IADDW:
		arith(i, 0x03, 0);
		break;
	case IADDL:
		arithl(i, 0x03);
		break;
	case IADDB:
		arithb(i, 0x02);
		break;
	case IADDF:
		arithf(i, Oaddf);
		break;
	case IORW:
		arith(i, 0x0b, 1);
		break;
	case IORL:
		arithl(i, 0x0b);
		break;
	case IORB:
		arithb(i, 0x0a);
		break;
	case IANDW:
		arith(i, 0x23, 4);
		break;
	case IANDL:
		arithl(i, 0x23);
		break;
	case IANDB:
		arithb(i, 0x22);
		break;
	case IXORW:
		arith(i, 0x33, 6);
		break;
	case IXORL:
		arithl(i, 0x33);
		break;
	case IXORB:
		arithb(i, 0x32);
		break;
	case ISHLW:
		shift(i, Oldl, Ostq, 0, 4);
		break;
	case ISHLL:
		shift(i, Oldq, Ostq, W, 4);
		break;
	case ISHLB:
		shift(i, Oldzb, Ostb, 0, 4);
		break;
	case ISHRW:
		shift(i, Oldl, Ostq, 0, 7);
		break;
	case ISHRL:
		shift(i, Oldq, Ostq, W, 7);
		break;
	case ISHRB:
		shift(i, Oldzb, Ostb, 0, 5);
		break;
	case ILSRW:
		shift(i, Oldl, Ostq, 0, 5);
		break;
	case ILSRL:
		shift(i, Oldq, Ostq, W, 5);
		break;
	case IMULF:
		arithf(i, Omulf);
		break;
	case IDIVF:
		arithf(i, Odivf);
		break;
	case INEGF:
		opwld(i, Oldq, RAX);
		modrr(W|ESC|0xba, RAX, 7);		/* BTCQ $63, AX */
		genb(63);
		opwst(i, Ostq, RAX);
		break;
	case IMOVB:
		opwld(i, Oldb, RAX);
		opwst(i, Ostb, RAX);
		break;
	case IMOVW:
		if(UXSRC(i->add) == SRC(AIMM)) {
			opwst(i, Omoviq, 0);
			genw(i->s.imm);
			break;
		}
		/* no break */
	case IMOVL:
	case IMOVF:
	case ICVTWL:
		opwld(i, Oldq, RAX);
		opwst(i, Ostq, RAX);
		break;
	case ICVTLW:
		opwld(i, Oldsx, RAX);
		opwst(i, Ostq, RAX);
		break;
	case ICALL:
		if(UXDST(i->add) != DST(AIMM))
			opwst(i, Oldq, RSI);
		opwld(i, Oldq, RAX);
		leacode(patch[i-mod->prog+1], RCX);
		modrm(Ostq, O(Frame, lr), RAX, RCX);	/* f->lr = next pc */
		modrm(Ostq, O(Frame, fp), RAX, RFP); 	/* f->fp = R.FP */
		modrr(Ostq, RFP, RAX);			/* R.FP = f */
		if(UXDST(i->add) != DST(AIMM)){
			gen2(0xff, (3<<6)|(4<<3)|RSI);	/* JMP* SI */
			break;
		}
		/* no break */
	case IJMP:
		if(RESCHED)
			schedcheck(i);
		rbra(patch[i->d.ins-mod->prog], Ojmp);
		break;
	case IMOVPC:
		leacode(patch[i->s.imm], RAX);
		opwst(i, Ostq, RAX);
		break;
	case IGOTO:
		opwst(i, Olea, RSI);
		opwld(i, Oldsx, RAX);
		modsib(0xff, 0, RSI, RAX, 3, 4);	/* JMP* (SI)(AX*8) */

		if(pass == 0)
			break;

		t = (WORD*)(mod->origmp+i->d.ind);
		e = t + t[-1];
		t[-1] = 0;
		while(t < e) {
			t[0] = (WORD)(base + patch[t[0]]);
			t++;
		}
		break;
	case IMULW:
		mid(i, Oldl, RAX);
		if(UXSRC(i->add) == SRC(AIMM)) {
			con(i->s.imm, RCX);
			modrr(Oimul, RCX, RAX);
		}
		else
			opwld(i, Oimul, RAX);
		modrr(Oldsx, RAX, RAX);
		opwst(i, Ostq, RAX);
		break;
	case IMULL:
		mid(i, Oldq, RAX);
		if(UXSRC(i->add) == SRC(AIMM)) {
			con(i->s.imm, RCX);
			modrr(W|Oimul, RCX, RAX);
		}
		else
			opwld(i, W|Oimul, RAX);
		opwst(i, Ostq, RAX);
		break;
	case IMODW:
	case IDIVW:
		/* divide in 64 bits, so the most negative int divided by -1 does not trap */
		mid(i, Oldsx, RAX);
		opwld(i, Oldsx, RCX);
		gen2(0x48, 0x99);			/* CQO */
		modrr(W|0xf7, RCX, 7);			/* IDIVQ CX */
		if(i->op == IMODW)
			modrr(Ostq, RAX, RDX);
		modrr(Oldsx, RAX, RAX);
		opwst(i, Ostq, RAX);
		break;
	case IMODL:
	case IDIVL:
		mid(i, Oldq, RAX);
		opwld(i, Oldq, RCX);
		gen2(0x48, 0x99);			/* CQO */
		modrr(W|0xf7, RCX, 7);			/* IDIVQ CX */
		if(i->op == IMODL)
			modrr(Ostq, RAX, RDX);
		opwst(i, Ostq, RAX);
		break;
	case IINDX:
		opwld(i, Oldq, RSI);			/* array */
		opwst(i, Oldl, RAX);			/* index */
		if(bflag){
			cmph(RSI);			/* a nil array has no elements */
			l1 = jccb(CCE);
			modrm(0x3b, O(Array, len), RSI, RAX);	/* CMPL AX, len */
			l = jccb(CCB);
			label(l1);
			cbounds(i);
			label(l);
		}
		modrm(Oldq, O(Array, t), RSI, RCX);
		modrm(Oldsx, O(Type, size), RCX, RCX);
		modrr(W|Oimul, RCX, RAX);		/* IMULQ CX, AX */
		modrm(W|0x03, O(Array, data), RSI, RAX);	/* ADDQ data, AX */
		midst(i, RAX);
		break;
	case IINDB:
		r = 0;
		goto idx;
	case IINDF:
	case IINDL:
	case IINDW:
		r = 3;
	idx:
		opwld(i, Oldq, RSI);			/* array */
		opwst(i, Oldl, RAX);			/* index */
		if(bflag){
			cmph(RSI);			/* a nil array has no elements */
			l1 = jccb(CCE);
			modrm(0x3b, O(Array, len), RSI, RAX);	/* CMPL AX, len */
			l = jccb(CCB);
			label(l1);
			cbounds(i);
			label(l);
		}
		modrm(Oldq, O(Array, data), RSI, RSI);
		modsib(Olea, 0, RSI, RAX, r, RAX);	/* LEA (SI)(AX*size), AX */
		midst(i, RAX);
		break;
	case IINDC:
		opwld(i, Oldq, RSI);			/* string */
		mid(i, Oldl, RAX);			/* index */
		modrm(Oldl, O(String, len), RSI, RCX);
		modrr(0x85, RCX, RCX);			/* TESTL CX, CX */
		l = jccb(CCL);
		if(bflag){
			modrr(0x3b, RCX, RAX);		/* CMPL AX, CX */
			l1 = jccb(CCB);
			cbounds(i);
			label(l1);
		}
		modsib(Oldzb, O(String, data), RSI, RAX, 0, RAX);
		l1 = jmpb();
		label(l);
		if(bflag){
			modrr(0xf7, RCX, 3);		/* NEGL CX */
			modrr(0x3b, RCX, RAX);		/* CMPL AX, CX */
			l = jccb(CCB);
			cbounds(i);
			label(l);
		}
		modsib(Oldl, O(String, data), RSI, RAX, 2, RAX);	/* sizeof(Rune) == 4 */
		label(l1);
		opwst(i, Ostq, RAX);
		break;
	case IRAISE:
		punt(i, SRCOP|WRTPC|NEWPC, optab[i->op]);
		break;
	case IMULX:
	case IDIVX:
	case ICVTXX:
	case IMULX0:
	case IDIVX0:
	case ICVTXX0:
	case IMULX1:
	case IDIVX1:
	case ICVTXX1:
	case ICVTFX:
	case ICVTXF:
	case IEXPW:
	case IEXPL:
	case IEXPF:
		punt(i, SRCOP|DSTOP|THREOP, optab[i->op]);
		break;
	case ISELF:
		punt(i, DSTOP, optab[i->op]);
		break;
	}
}

static void
preamble(void)
{
	if(comvec)
		return;

	comvec = malloc(64);
	if(comvec == nil)
		error(exNomem);
	code = (uchar*)comvec;

	/* five pushes leave the stack aligned for calls to C */
	genb(Opush+RBX);
	opcode(Opush+(R12&7), 0, 0, R12);
	opcode(Opush+(R13&7), 0, 0, R13);
	opcode(Opush+(R14&7), 0, 0, R14);
	opcode(Opush+(R15&7), 0, 0, R15);
	conp(&R, RREG);
	modrm(Oldq, O(REG, FP), RREG, RFP);
	modrm(Oldq, O(REG, MP), RREG, RMP);
	modrm(0xff, O(REG, PC), RREG, 4);	/* JMP* R.PC */

	segflush(comvec, 64);
}

/*
 * decrement the reference count of the object at AX, and free it at zero
 */
static void
macfrp(void)
{
	uchar *l;

	cmph(RAX);
	l = jccb(CCNE);
	genb(Oret);
	label(l);
	modrm(0x83, HREF, RAX, 7);		/* CMPL ref(AX), $1 */
	genb(0x01);
	l = jccb(CCE);
	modrm(0xff, HREF, RAX, 1);		/* DECL ref(AX) */
	genb(Oret);
	label(l);
	modrm(Ostq, O(REG, FP), RREG, RFP);
	modrm(Ostq, O(REG, s), RREG, RAX);
	ccall(rdestroy);
	modrm(Oldq, O(REG, FP), RREG, RFP);
	modrm(Oldq, O(REG, MP), RREG, RMP);
	genb(Oret);
}

static void
macret(void)
{
	Inst i;
	uchar *lpunt[4], *lnomr, *lfrmr, *linterp;
	int n;

	n = 0;
	modrm(Oldq, O(Frame, t), RFP, RAX);
	cmpi(RAX, 0);
	lpunt[n++] = jccb(CCE);
	modrm(Oldq, O(Type, destroy), RAX, RAX);
	cmpi(RAX, 0);
	lpunt[n++] = jccb(CCE);
	modrm(W|0x83, O(Frame, fp), RFP, 7);	/* CMPQ fp(FP), $0 */
	genb(0);
	lpunt[n++] = jccb(CCE);
	modrm(W|0x83, O(Frame, mr), RFP, 7);	/* CMPQ mr(FP), $0 */
	genb(0);
	lnomr = jccb(CCE);
	modrm(Oldq, O(REG, M), RREG, RTA);
	modrm(0xff, HREF, RTA, 1);		/* DECL ref(R.M) */
	lfrmr = jccb(CCNE);
	modrm(0xff, HREF, RTA, 0);		/* INCL ref(R.M) */
	lpunt[n++] = jmpb();
	label(lfrmr);
	modrm(Oldq, O(Frame, mr), RFP, RTA);
	modrm(Ostq, O(REG, M), RREG, RTA);
	modrm(Oldq, O(Modlink, MP), RTA, RMP);
	modrm(Ostq, O(REG, MP), RREG, RMP);
	modrm(0x83, O(Modlink, compiled), RTA, 7);	/* CMPL compiled, $0 */
	genb(0x00);
	linterp = jccb(CCE);
	label(lnomr);
	gen2(0xff, (3<<6)|(2<<3)|RAX);		/* CALL* AX: the frame's destroy code */
	modrm(Ostq, O(REG, SP), RREG, RFP);
	modrm(Oldq, O(Frame, lr), RFP, RAX);
	modrm(Oldq, O(Frame, fp), RFP, RFP);
	modrm(Ostq, O(REG, FP), RREG, RFP);
	gen2(0xff, (3<<6)|(4<<3)|RAX);		/* JMP* AX */

	label(linterp);				/* return to uncompiled code */
	gen2(0xff, (3<<6)|(2<<3)|RAX);		/* CALL* AX */
	modrm(Ostq, O(REG, SP), RREG, RFP);
	modrm(Oldq, O(Frame, lr), RFP, RAX);
	modrm(Ostq, O(REG, PC), RREG, RAX);
	modrm(Oldq, O(Frame, fp), RFP, RFP);
	modrm(Ostq, O(REG, FP), RREG, RFP);
	leave();

	while(n > 0)
		label(lpunt[--n]);
	i.add = AXNON;
	punt(&i, TCHECK|NEWPC, optab[IRET]);
}

/*
 * increment the reference count of the object at SI, and mark it for the collector
 */
static void
maccolr(void)
{
	uchar *l;

	modrm(0xff, HREF, RSI, 0);		/* INCL ref(SI) */
	conp(&mutator, RAX);
	modrm(Oldl, 0, RAX, RAX);
	modrm(0x39, HCOLOR, RSI, RAX);		/* CMPL color(SI), AX */
	l = jccb(CCNE);
	genb(Oret);
	label(l);
	modrm(Omovil, HCOLOR, RSI, 0);		/* MOVL $propagator, color(SI) */
	genw(propagator);
	conp(&nprop, RAX);
	modrm(Omovil, 0, RAX, 0);		/* MOVL $1, nprop */
	genw(1);
	genb(Oret);
}

/*
 * call through a module link: CX has the new frame,
 * RTA the Modlink, and AX the pc of the function (unless RTA is nil)
 */
static void
macmcal(void)
{
	uchar *label1, *mlnil, *interp;

	cmph(RTA);
	mlnil = jccb(CCE);
	modrm(W|0x83, O(Modlink, prog), RTA, 7);	/* CMPQ ml->prog, $0 */
	genb(0x00);
	label1 = jccb(CCNE);
	label(mlnil);				/* built-in module, or nil */
	modrm(Ostq, O(REG, FP), RREG, RCX);
	cmph(RTA);
	mlnil = jccb(CCNE);
	con(-1, RAX);				/* rmcall will raise the exception */
	label(mlnil);
	modrm(Ostq, O(REG, dt), RREG, RAX);
	ccall(rmcall);
	modrm(Oldq, O(REG, FP), RREG, RFP);
	modrm(Oldq, O(REG, MP), RREG, RMP);
	genb(Oret);
	label(label1);
	modrr(Ostq, RFP, RCX);			/* R.FP = f */
	modrm(Ostq, O(REG, M), RREG, RTA);	/* R.M = ml */
	modrm(0xff, HREF, RTA, 0);		/* INCL ref(ml) */
	modrm(Oldq, O(Modlink, MP), RTA, RMP);
	modrm(Ostq, O(REG, MP), RREG, RMP);	/* R.MP = ml->MP */
	modrm(0x83, O(Modlink, compiled), RTA, 7);	/* CMPL compiled, $0 */
	genb(0x00);
	genb(Opop+RTA);				/* balance call */
	interp = jccb(CCE);
	gen2(0xff, (3<<6)|(4<<3)|RAX);		/* JMP* AX */
	label(interp);				/* call to uncompiled code */
	modrm(Ostq, O(REG, FP), RREG, RFP);
	modrm(Ostq, O(REG, PC), RREG, RAX);
	leave();
}

/*
 * allocate a frame of the type at RTA, leaving its address in CX
 */
static void
macfram(void)
{
	uchar *l;

	modrm(Oldq, O(REG, SP), RREG, RAX);
	modrm(Oldsx, O(Type, size), RTA, RCX);
	modrr(W|0x03, RCX, RAX);		/* ADDQ CX, AX */
	modrm(W|0x3b, O(REG, TS), RREG, RAX);	/* CMPQ AX, R.TS */
	l = jccb(CCB);

	modrm(Ostq, O(REG, s), RREG, RTA);
	modrm(Ostq, O(REG, FP), RREG, RFP);
	ccall(extend);
	modrm(Oldq, O(REG, FP), RREG, RFP);
	modrm(Oldq, O(REG, MP), RREG, RMP);
	modrm(Oldq, O(REG, s), RREG, RCX);
	genb(Oret);

	label(l);
	modrm(Oldq, O(REG, SP), RREG, RCX);
	modrm(Ostq, O(REG, SP), RREG, RAX);
	modrm(Ostq, O(Frame, t), RCX, RTA);	/* f->t = t */
	modrm(Omoviq, O(Frame, mr), RCX, 0);	/* f->mr = nil */
	genw(0);
	modrm(Oldq, O(Type, initialize), RTA, RTA);
	gen2(0xff, (3<<6)|(4<<3)|RTA);		/* JMP* RTA: it returns for us */
}

static void
macmfra(void)
{
	modrm(Ostq, O(REG, FP), RREG, RFP);
	modrm(Ostq, O(REG, s), RREG, RAX);	/* Save type */
	modrm(Ostq, O(REG, d), RREG, RTA);	/* Save destination */
	ccall(rmfram);
	modrm(Oldq, O(REG, FP), RREG, RFP);
	modrm(Oldq, O(REG, MP), RREG, RMP);
	genb(Oret);
}

static void
macrelq(void)
{
	modrm(Ostq, O(REG, FP), RREG, RFP);
	genb(Opop+RAX);
	modrm(Ostq, O(REG, PC), RREG, RAX);
	leave();
}

/*
 * binary search of the case table at SI for the value in AX:
 * a count n, then n entries of low value, high value+1 and code address,
 * then the address for the default (see icase in xec.c)
 */
static void
maccase(void)
{
	uchar *loop, *done, *found, *l;

	modrm(Oldq, 0, RSI, RCX);		/* n = t[0] */
	modrm(Olea, IBY2WD, RSI, RSI);		/* t = &t[1] */
	modsib(Olea, 0, RCX, RCX, 1, RDI);
	modsib(Oldq, 0, RSI, RDI, 3, RDI);	/* DI = t[n*3], the default */
	loop = code;
	cmpi(RCX, 0);
	done = jccb(CCLE);
	modrr(Ostq, RDX, RCX);			/* n2 = n */
	modrr(W|0xd1, RDX, 5);			/* n2 >>= 1 */
	modsib(Olea, 0, RDX, RDX, 1, R8);
	modsib(Olea, 0, RSI, R8, 3, R8);	/* l = &t[n2*3] */
	modrm(W|0x3b, 0, R8, RAX);		/* CMPQ AX, l[0] */
	l = jccb(CCGE);
	modrr(Ostq, RCX, RDX);			/* n = n2 */
	gen2(Ojmpb, loop-code-2);
	label(l);
	modrm(W|0x3b, IBY2WD, R8, RAX);		/* CMPQ AX, l[1] */
	found = jccb(CCL);
	modrm(Olea, 3*IBY2WD, R8, RSI);		/* t = l+3 */
	modrr(W|0x2b, RDX, RCX);		/* n -= n2 */
	modrr(W|0xff, RCX, 1);			/* n -= 1 */
	gen2(Ojmpb, loop-code-2);
	label(found);
	modrm(0xff, 2*IBY2WD, R8, 4);		/* JMP* l[2] */
	label(done);
	gen2(0xff, (3<<6)|(4<<3)|RDI);		/* JMP* DI */
}

/* code to destroy the pointers in a frame of type t at RFP */
void
comd(Type *t)
{
	int i, j, m, c;

	for(i = 0; i < t->np; i++) {
		c = t->map[i];
		j = i*8*sizeof(WORD*);
		for(m = 0x80; m != 0; m >>= 1) {
			if(c & m) {
				modrm(Oldq, j, RFP, RAX);
				rbra(macro[MacFRP], Ocall);
			}
			j += sizeof(WORD*);
		}
	}
	genb(Oret);
}

/* code to initialise the pointers in a frame of type t at CX */
void
comi(Type *t)
{
	int i, j, m, c;

	con(-1, RAX);		/* H */
	for(i = 0; i < t->np; i++) {
		c = t->map[i];
		j = i*8*sizeof(WORD*);
		for(m = 0x80; m != 0; m >>= 1) {
			if(c & m)
				modrm(Ostq, j, RCX, RAX);
			j += sizeof(WORD*);
		}
	}
	genb(Oret);
}

/*
 * the code for a type is separate from the module's, but calls its
 * macros with relative branches, so both passes must agree on base.
 */
void
typecom(Type *t)
{
	int n;
	uchar *tmp, *start;

	if(t == nil || t->initialize != 0)
		return;

	tmp = mallocz(4096*sizeof(uchar), 0);
	if(tmp == nil)
		error(exNomem);

	code = tmp;
	comi(t);
	n = code - tmp;
	code = tmp;
	comd(t);
	n += code - tmp;
	free(tmp);

	start = mallocz(n, 0);
	if(start == nil)
		return;
	code = start;

	t->initialize = code;
	comi(t);
	t->destroy = code;
	comd(t);

	if(cflag > 3)
		print("typ= %p %4d i %p d %p asm=%d\n",
			t, t->size, t->initialize, t->destroy, n);

	segflush(start, n);
}

static void
patchex(Module *m, ulong *p)
{
	Handler *h;
	Except *e;

	if((h = m->htab) == nil)
		return;
	for( ; h->etab != nil; h++){
		h->pc1 = p[h->pc1];
		h->pc2 = p[h->pc2];
		for(e = h->etab; e->s != nil; e++)
			e->pc = p[e->pc];
		if(e->pc != -1)
			e->pc = p[e->pc];
	}
}

int
compile(Module *m, int size, Modlink *ml)
{
	Modl *e;
	Link *l;
	int i, n;
	uchar *s, *tmp;

	base = nil;
	patch = mallocz((size+1)*sizeof(*patch), 0);	/* one more for the end of the code */
	tinit = malloc(m->ntype*sizeof(*tinit));
	tmp = mallocz(4096*sizeof(uchar),0);
	if(tinit == nil || patch == nil || tmp == nil)
		goto bad;

	preamble();

	mod = m;
	n = 0;
	pass = 0;
	nlit = 0;
	litpool = nil;
	memset(tinit, 0, m->ntype*sizeof(*tinit));

	/*
	 * the first pass finds the size of the code for each instruction;
	 * branch displacements are wrong, but have the right size.
	 */
	for(i = 0; i < size; i++) {
		base = code = tmp;
		comp(&m->prog[i]);
		patch[i] = n;
		n += code - tmp;
	}
	patch[size] = n;

	for(i = 0; i < nelem(mactab); i++) {
		base = code = tmp;
		mactab[i].gen();
		macro[mactab[i].idx] = n;
		n += code - tmp;
	}

	n = (n+7)&~7;

	base = mallocz(n + nlit*sizeof(WORD), 0);
	if(base == nil)
		goto bad;

	if(cflag > 3)
		print("dis=%5d %5d amd64=%5d asm=%p lit=%d: %s\n",
			size, (int)(size*sizeof(Inst)), n, base, nlit, m->name);

	pass++;
	nlit = 0;
	litpool = (WORD*)(base+n);
	code = base;

	for(i = 0; i < size; i++) {
		s = code;
		comp(&m->prog[i]);
		if(code-base != (i+1 < size? patch[i+1]: code-base)) {
			print("%s: compile phase error at %D\n", m->name, &m->prog[i]);
			urk();
		}
		if(cflag > 4) {
			print("%D\n", &m->prog[i]);
			das(s, code-s);
		}
	}

	for(i = 0; i < nelem(mactab); i++) {
		if(code-base != macro[mactab[i].idx]) {
			print("%s: compile phase error at macro %d\n", m->name, i);
			urk();
		}
		mactab[i].gen();
	}

	for(l = m->ext; l->name; l++) {
		l->u.pc = (Inst*)(base+patch[l->u.pc-m->prog]);
		typecom(l->frame);
	}
	if(ml != nil) {
		e = &ml->links[0];
		for(i = 0; i < ml->nlinks; i++) {
			e->u.pc = (Inst*)(base+patch[e->u.pc-m->prog]);
			typecom(e->frame);
			e++;
		}
	}
	for(i = 0; i < m->ntype; i++) {
		if(tinit[i] != 0)
			typecom(m->type[i]);
	}
	patchex(m, patch);
	m->entry = (Inst*)(base+patch[mod->entry-mod->prog]);
	m->pctab = patch;	/* size+1 offsets: maps code back to Dis pcs, for faults and the debugger */
	free(tinit);
	free(tmp);
	free(m->prog);
	m->prog = (Inst*)base;
	m->ncode = n;
	m->compiled = 1;
	segflush(base, n + nlit*sizeof(WORD));
	return 1;
bad:
	free(patch);
	free(tinit);
	free(tmp);
	free(base);
	return 0;
}
