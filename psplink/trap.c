/*
 * Copyright (c) 1988 University of Utah.
 *
 * This code is derived from software contributed to Berkeley by
 * the Systems Programming Group of the University of Utah Computer
 * Science Department and Ralph Campbell.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *
 *	@(#)trap.c	8.1 (Berkeley) 6/10/93
 */
 
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <pthread.h>
#include "trap.h"

#define intptr_t int32_t

/*
 *  stack trace code, also useful to DDB one day
 */

#define	MIPS_JR_RA	0x03e00008	/* instruction code for jr ra */
#define	MIPS_JR_K0	0x03400008	/* instruction code for jr k0 */
#define	MIPS_ERET	0x42000018	/* instruction code for eret */

unsigned int verylocore = 0x400000; //0x20000000;


int
kdbpeek(vaddr_t addr)
{
	int rc;

	if (addr & 3) {
		printf("kdbpeek: unaligned address 0x%x\n", addr);
		/* We might have been called from DDB, so do not go there. */
		//stacktrace();
		rc = -1 ;
	} else if (addr == 0) {
		printf("kdbpeek: NULL\n");
		rc = 0xdeadfeed;
	} else {
		rc = *(int *)addr;
	}
	return rc;
}

mips_reg_t
kdbrpeek(vaddr_t addr, size_t n)
{
	mips_reg_t rc;

	if (addr & (n - 1)) {
		printf("kdbrpeek: unaligned address 0x%x\n", addr);
		/* We might have been called from DDB, so do not go there. */
		//stacktrace();
		rc = -1 ;
	} else if (addr == 0) {
		printf("kdbrpeek: NULL\n");
		rc = 0xdeadfeed;
	} else {
		if (sizeof(mips_reg_t) == 8 && n == 8)
			rc = *(int64_t *)addr;
		else
			rc = *(int32_t *)addr;
	}
	return rc;
}

/*
 * Do a stack backtrace.
 * (*printfn)()  prints the output to either the system log,
 * the console, or both.
 */
void
stacktrace_subr(mips_reg_t a0, mips_reg_t a1, mips_reg_t a2, mips_reg_t a3,
    vaddr_t pc, vaddr_t sp, vaddr_t fp, vaddr_t ra,
    void (*printfn)(const char*, ...))
{
	vaddr_t va, subr;
	unsigned instr, mask;
	InstFmt i;
	int more, stksize;
	unsigned int frames =  0;
	int foundframesize = 0;
#ifdef DDB
	db_expr_t diff;
	db_sym_t sym;
#endif

/* Jump here when done with a frame, to start a new one */
loop:
	stksize = 0;
	subr = 0;
	if (frames++ > 100) {
		(*printfn)("\nstackframe count exceeded\n");
		/* return breaks stackframe-size heuristics with gcc -O2 */
		goto finish;	/*XXX*/
	}

	/* check for bad SP: could foul up next frame */
	//if (sp & 3 || (intptr_t)sp >= 0) {
	if (sp & 3 ) {
		(*printfn)("SP 0x%x: not in kernel\n", sp);
		ra = 0;
		subr = 0;
		goto done;
	}

	/* Check for bad PC */
	//if (pc & 3 || (intptr_t)pc >= 0 || (intptr_t)pc >= (intptr_t)edata) {
	//if (pc & 3 || (intptr_t)pc >= 0) {
	if (pc & 3) {
		(*printfn)("PC 0x%x: not in kernel space\n", pc);
		ra = 0;
		goto done;
	}


#ifdef DDB
	/*
	 * Check the kernel symbol table to see the beginning of
	 * the current subroutine.
	 */
	diff = 0;
	sym = db_search_symbol(pc, DB_STGY_ANY, &diff);
	if (sym != DB_SYM_NULL && diff == 0) {
		/* check func(foo) __attribute__((__noreturn__)) case */
		instr = kdbpeek(pc - 2 * sizeof(int));
		i.word = instr;
		if (i.JType.op == OP_JAL) {
			sym = db_search_symbol(pc - sizeof(int),
			    DB_STGY_ANY, &diff);
			if (sym != DB_SYM_NULL && diff != 0)
				diff += sizeof(int);
		}
	}
	if (sym == DB_SYM_NULL) {
		ra = 0;
		goto done;
	}
	va = pc - diff;
#else
	/*
	 * Find the beginning of the current subroutine by scanning backwards
	 * from the current PC for the end of the previous subroutine.
	 * 
	 * XXX This won't work well because nowadays gcc is so aggressive
	 *     as to reorder instruction blocks for branch-predict.
	 *     (i.e. 'jr ra' wouldn't indicate the end of subroutine)
	 */
	va = pc;
	do {
		va -= sizeof(int);
		if (va <= (vaddr_t)verylocore)
			goto finish;
		instr = kdbpeek(va);
		if (instr == MIPS_ERET)
			goto mips3_eret;
	} while (instr != MIPS_JR_RA && instr != MIPS_JR_K0);
	/* skip back over branch & delay slot */
	va += sizeof(int);
mips3_eret:
	va += sizeof(int);
	/* skip over nulls which might separate .o files */
	while ((instr = kdbpeek(va)) == 0)
		va += sizeof(int);
#endif
	subr = va;

	/* scan forwards to find stack size and any saved registers */
	stksize = 0;
	more = 3;
	mask = 0;
	foundframesize = 0;
	for (va = subr; more; va += sizeof(int),
			      more = (more == 3) ? 3 : more - 1) {
		/* stop if hit our current position */
		if (va >= pc)
			break;
		instr = kdbpeek(va);
		i.word = instr;
		switch (i.JType.op) {
		case OP_SPECIAL:
			switch (i.RType.func) {
			case OP_JR:
			case OP_JALR:
				more = 2; /* stop after next instruction */
				break;

			case OP_SYSCALL:
			case OP_BREAK:
				more = 1; /* stop now */
			};
			break;

		case OP_BCOND:
		case OP_J:
		case OP_JAL:
		case OP_BEQ:
		case OP_BNE:
		case OP_BLEZ:
		case OP_BGTZ:
			more = 2; /* stop after next instruction */
			break;

		case OP_COP0:
		case OP_COP1:
		case OP_COP2:
		case OP_COP3:
			switch (i.RType.rs) {
			case OP_BCx:
			case OP_BCy:
				more = 2; /* stop after next instruction */
			};
			break;

		case OP_SW:
#if !defined(__mips_o32)
		case OP_SD:
#endif
		{
			size_t size = (i.JType.op == OP_SW) ? 4 : 8;

			/* look for saved registers on the stack */
			if (i.IType.rs != 29)
				break;
			/* only restore the first one */
			if (mask & (1 << i.IType.rt))
				break;
			mask |= (1 << i.IType.rt);
			switch (i.IType.rt) {
			case 4: /* a0 */
				a0 = kdbrpeek(sp + (short)i.IType.imm, size);
				break;

			case 5: /* a1 */
				a1 = kdbrpeek(sp + (short)i.IType.imm, size);
				break;

			case 6: /* a2 */
				a2 = kdbrpeek(sp + (short)i.IType.imm, size);
				break;

			case 7: /* a3 */
				a3 = kdbrpeek(sp + (short)i.IType.imm, size);
				break;

			case 30: /* fp */
				fp = kdbrpeek(sp + (short)i.IType.imm, size);
				break;

			case 31: /* ra */
				ra = kdbrpeek(sp + (short)i.IType.imm, size);
			}
			break;
		}

		case OP_ADDI:
		case OP_ADDIU:
#if !defined(__mips_o32)
		case OP_DADDI:
		case OP_DADDIU:
#endif
			/* look for stack pointer adjustment */
			if (i.IType.rs != 29 || i.IType.rt != 29)
				break;
			/* don't count pops for mcount */
			if (!foundframesize) {
				stksize = - ((short)i.IType.imm);
				foundframesize = 1;
			}
		}
	}
done:
	(*printfn)("%x(%x,%x,%x,%x) pc [%x] ra [%x] sz [%d]\n",
		subr, a0, a1, a2, a3, pc, ra, stksize);

	if (ra) {
		if (pc == ra && stksize == 0)
			(*printfn)("stacktrace: loop!\n");
		else {
			pc = ra;
			sp += stksize;
			ra = 0;
			goto loop;
		}
	} else {
finish:
		(*printfn)("done!\n");
	}
}

