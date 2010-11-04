/*
 * Copyright (C) 1995-2008 University of Karlsruhe.  All right reserved.
 *
 * This file is part of libFirm.
 *
 * This file may be distributed and/or modified under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation and appearing in the file LICENSE.GPL included in the
 * packaging of this file.
 *
 * Licensees holding valid libFirm Professional Edition licenses may use
 * this file in accordance with the libFirm Commercial License.
 * Agreement provided with the Software.
 *
 * This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
 * WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE.
 */

/**
 * @file
 * @brief       This file implements the ia32 node emitter.
 * @author      Christian Wuerdig, Matthias Braun
 * @version     $Id$
 *
 * Summary table for x86 floatingpoint compares:
 *   pnc_Eq  => !P && E
 *   pnc_Lt  => !P && B
 *   pnc_Le  => !P && BE
 *   pnc_Gt  => A
 *   pnc_Ge  => AE
 *   pnc_Lg  => P || NE
 *   pnc_Leg => NP  (ordered)
 *   pnc_Uo  => P
 *   pnc_Ue  => E
 *   pnc_Ul  => B
 *   pnc_Ule => BE
 *   pnc_Ug  => P || A
 *   pnc_Uge => P || AE
 *   pnc_Ne  => NE
 */
#include "config.h"

#include <limits.h>

#include "xmalloc.h"
#include "tv.h"
#include "iredges.h"
#include "debug.h"
#include "irgwalk.h"
#include "irprintf.h"
#include "irop_t.h"
#include "irargs_t.h"
#include "irprog_t.h"
#include "iredges_t.h"
#include "irtools.h"
#include "execfreq.h"
#include "error.h"
#include "raw_bitset.h"
#include "dbginfo.h"
#include "lc_opts.h"

#include "../besched.h"
#include "../benode.h"
#include "../beabi.h"
#include "../be_dbgout.h"
#include "../beemitter.h"
#include "../begnuas.h"
#include "../beirg.h"
#include "../be_dbgout.h"

#include "ia32_emitter.h"
#include "ia32_common_transform.h"
#include "gen_ia32_emitter.h"
#include "gen_ia32_regalloc_if.h"
#include "ia32_nodes_attr.h"
#include "ia32_new_nodes.h"
#include "ia32_map_regs.h"
#include "ia32_architecture.h"
#include "bearch_ia32_t.h"

DEBUG_ONLY(static firm_dbg_module_t *dbg = NULL;)

#define SNPRINTF_BUF_LEN 128

static const ia32_isa_t *isa;
static char              pic_base_label[128];
static ir_label_t        exc_label_id;
static int               mark_spill_reload = 0;
static int               do_pic;

/** Return the next block in Block schedule */
static ir_node *get_prev_block_sched(const ir_node *block)
{
	return (ir_node*)get_irn_link(block);
}

/** Checks if the current block is a fall-through target. */
static int is_fallthrough(const ir_node *cfgpred)
{
	ir_node *pred;

	if (!is_Proj(cfgpred))
		return 1;
	pred = get_Proj_pred(cfgpred);
	if (is_ia32_SwitchJmp(pred))
		return 0;

	return 1;
}

/**
 * returns non-zero if the given block needs a label
 * because of being a jump-target (and not a fall-through)
 */
static int block_needs_label(const ir_node *block)
{
	int need_label = 1;
	int  n_cfgpreds = get_Block_n_cfgpreds(block);

	if (has_Block_entity(block))
		return 1;

	if (n_cfgpreds == 0) {
		need_label = 0;
	} else if (n_cfgpreds == 1) {
		ir_node *cfgpred       = get_Block_cfgpred(block, 0);
		ir_node *cfgpred_block = get_nodes_block(cfgpred);

		if (get_prev_block_sched(block) == cfgpred_block
				&& is_fallthrough(cfgpred)) {
			need_label = 0;
		}
	}

	return need_label;
}

/**
 * Returns the register at in position pos.
 */
static const arch_register_t *get_in_reg(const ir_node *irn, int pos)
{
	ir_node               *op;
	const arch_register_t *reg = NULL;

	assert(get_irn_arity(irn) > pos && "Invalid IN position");

	/* The out register of the operator at position pos is the
	   in register we need. */
	op = get_irn_n(irn, pos);

	reg = arch_get_irn_register(op);

	assert(reg && "no in register found");

	if (reg == &ia32_registers[REG_GP_NOREG])
		panic("trying to emit noreg for %+F input %d", irn, pos);

	return reg;
}

/**
 * Returns the register at out position pos.
 */
static const arch_register_t *get_out_reg(const ir_node *irn, int pos)
{
	ir_node               *proj;
	const arch_register_t *reg = NULL;

	/* 1st case: irn is not of mode_T, so it has only                 */
	/*           one OUT register -> good                             */
	/* 2nd case: irn is of mode_T -> collect all Projs and ask the    */
	/*           Proj with the corresponding projnum for the register */

	if (get_irn_mode(irn) != mode_T) {
		assert(pos == 0);
		reg = arch_get_irn_register(irn);
	} else if (is_ia32_irn(irn)) {
		reg = arch_irn_get_register(irn, pos);
	} else {
		const ir_edge_t *edge;

		foreach_out_edge(irn, edge) {
			proj = get_edge_src_irn(edge);
			assert(is_Proj(proj) && "non-Proj from mode_T node");
			if (get_Proj_proj(proj) == pos) {
				reg = arch_get_irn_register(proj);
				break;
			}
		}
	}

	assert(reg && "no out register found");
	return reg;
}

/**
 * Add a number to a prefix. This number will not be used a second time.
 */
static char *get_unique_label(char *buf, size_t buflen, const char *prefix)
{
	static unsigned long id = 0;
	snprintf(buf, buflen, "%s%s%lu", be_gas_get_private_prefix(), prefix, ++id);
	return buf;
}


/**
 * Emit the name of the 8bit low register
 */
static void emit_8bit_register(const arch_register_t *reg)
{
	const char *reg_name = arch_register_get_name(reg);

	be_emit_char('%');
	be_emit_char(reg_name[1]);
	be_emit_char('l');
}

/**
 * Emit the name of the 8bit high register
 */
static void emit_8bit_register_high(const arch_register_t *reg)
{
	const char *reg_name = arch_register_get_name(reg);

	be_emit_char('%');
	be_emit_char(reg_name[1]);
	be_emit_char('h');
}

static void emit_16bit_register(const arch_register_t *reg)
{
	const char *reg_name = ia32_get_mapped_reg_name(isa->regs_16bit, reg);

	be_emit_char('%');
	be_emit_string(reg_name);
}

/**
 * emit a register, possible shortened by a mode
 *
 * @param reg   the register
 * @param mode  the mode of the register or NULL for full register
 */
static void emit_register(const arch_register_t *reg, const ir_mode *mode)
{
	const char *reg_name;

	if (mode != NULL) {
		int size = get_mode_size_bits(mode);
		switch (size) {
			case  8: emit_8bit_register(reg);  return;
			case 16: emit_16bit_register(reg); return;
		}
		assert(mode_is_float(mode) || size == 32);
	}

	reg_name = arch_register_get_name(reg);

	be_emit_char('%');
	be_emit_string(reg_name);
}

void ia32_emit_source_register(const ir_node *node, int pos)
{
	const arch_register_t *reg = get_in_reg(node, pos);

	emit_register(reg, NULL);
}

static void ia32_emit_entity(ir_entity *entity, int no_pic_adjust)
{
	be_gas_emit_entity(entity);

	if (get_entity_owner(entity) == get_tls_type()) {
		if (get_entity_visibility(entity) == ir_visibility_external) {
			be_emit_cstring("@INDNTPOFF");
		} else {
			be_emit_cstring("@NTPOFF");
		}
	}

	if (do_pic && !no_pic_adjust) {
		be_emit_char('-');
		be_emit_string(pic_base_label);
	}
}

static void emit_ia32_Immediate_no_prefix(const ir_node *node)
{
	const ia32_immediate_attr_t *attr = get_ia32_immediate_attr_const(node);

	if (attr->symconst != NULL) {
		if (attr->sc_sign)
			be_emit_char('-');
		ia32_emit_entity(attr->symconst, attr->no_pic_adjust);
	}
	if (attr->symconst == NULL || attr->offset != 0) {
		if (attr->symconst != NULL) {
			be_emit_irprintf("%+d", attr->offset);
		} else {
			be_emit_irprintf("0x%X", attr->offset);
		}
	}
}

static void emit_ia32_Immediate(const ir_node *node)
{
	be_emit_char('$');
	emit_ia32_Immediate_no_prefix(node);
}

void ia32_emit_8bit_source_register_or_immediate(const ir_node *node, int pos)
{
	const arch_register_t *reg;
	const ir_node         *in = get_irn_n(node, pos);
	if (is_ia32_Immediate(in)) {
		emit_ia32_Immediate(in);
		return;
	}

	reg = get_in_reg(node, pos);
	emit_8bit_register(reg);
}

void ia32_emit_8bit_high_source_register(const ir_node *node, int pos)
{
	const arch_register_t *reg = get_in_reg(node, pos);
	emit_8bit_register_high(reg);
}

void ia32_emit_16bit_source_register_or_immediate(const ir_node *node, int pos)
{
	const arch_register_t *reg;
	const ir_node         *in = get_irn_n(node, pos);
	if (is_ia32_Immediate(in)) {
		emit_ia32_Immediate(in);
		return;
	}

	reg = get_in_reg(node, pos);
	emit_16bit_register(reg);
}

void ia32_emit_dest_register(const ir_node *node, int pos)
{
	const arch_register_t *reg  = get_out_reg(node, pos);

	emit_register(reg, NULL);
}

void ia32_emit_dest_register_size(const ir_node *node, int pos)
{
	const arch_register_t *reg  = get_out_reg(node, pos);

	emit_register(reg, get_ia32_ls_mode(node));
}

void ia32_emit_8bit_dest_register(const ir_node *node, int pos)
{
	const arch_register_t *reg  = get_out_reg(node, pos);

	emit_register(reg, mode_Bu);
}

void ia32_emit_x87_register(const ir_node *node, int pos)
{
	const ia32_x87_attr_t *attr = get_ia32_x87_attr_const(node);

	assert(pos < 3);
	be_emit_char('%');
	be_emit_string(attr->x87[pos]->name);
}

static void ia32_emit_mode_suffix_mode(const ir_mode *mode)
{
	assert(mode_is_int(mode) || mode_is_reference(mode));
	switch (get_mode_size_bits(mode)) {
		case 8:  be_emit_char('b');     return;
		case 16: be_emit_char('w');     return;
		case 32: be_emit_char('l');     return;
		/* gas docu says q is the suffix but gcc, objdump and icc use ll
		 * apparently */
		case 64: be_emit_cstring("ll"); return;
	}
	panic("Can't output mode_suffix for %+F", mode);
}

void ia32_emit_mode_suffix(const ir_node *node)
{
	ir_mode *mode = get_ia32_ls_mode(node);
	if (mode == NULL)
		mode = mode_Iu;

	ia32_emit_mode_suffix_mode(mode);
}

void ia32_emit_x87_mode_suffix(const ir_node *node)
{
	ir_mode *mode;

	/* we only need to emit the mode on address mode */
	if (get_ia32_op_type(node) == ia32_Normal)
		return;

	mode = get_ia32_ls_mode(node);
	assert(mode != NULL);

	if (mode_is_float(mode)) {
		switch (get_mode_size_bits(mode)) {
			case  32: be_emit_char('s'); return;
			case  64: be_emit_char('l'); return;
			/* long doubles have different sizes due to alignment on different
			 * platforms. */
			case  80:
			case  96:
			case 128: be_emit_char('t'); return;
		}
	} else {
		assert(mode_is_int(mode) || mode_is_reference(mode));
		switch (get_mode_size_bits(mode)) {
			case 16: be_emit_char('s');     return;
			case 32: be_emit_char('l');     return;
			/* gas docu says q is the suffix but gcc, objdump and icc use ll
			 * apparently */
			case 64: be_emit_cstring("ll"); return;
		}
	}
	panic("Can't output mode_suffix for %+F", mode);
}

static char get_xmm_mode_suffix(ir_mode *mode)
{
	assert(mode_is_float(mode));
	switch (get_mode_size_bits(mode)) {
	case 32: return 's';
	case 64: return 'd';
	default: panic("Invalid XMM mode");
	}
}

void ia32_emit_xmm_mode_suffix(const ir_node *node)
{
	ir_mode *mode = get_ia32_ls_mode(node);
	assert(mode != NULL);
	be_emit_char('s');
	be_emit_char(get_xmm_mode_suffix(mode));
}

void ia32_emit_xmm_mode_suffix_s(const ir_node *node)
{
	ir_mode *mode = get_ia32_ls_mode(node);
	assert(mode != NULL);
	be_emit_char(get_xmm_mode_suffix(mode));
}

void ia32_emit_extend_suffix(const ir_node *node)
{
	ir_mode *mode = get_ia32_ls_mode(node);
	if (get_mode_size_bits(mode) == 32)
		return;
	be_emit_char(mode_is_signed(mode) ? 's' : 'z');
	ia32_emit_mode_suffix_mode(mode);
}

void ia32_emit_source_register_or_immediate(const ir_node *node, int pos)
{
	ir_node *in = get_irn_n(node, pos);
	if (is_ia32_Immediate(in)) {
		emit_ia32_Immediate(in);
	} else {
		const ir_mode         *mode = get_ia32_ls_mode(node);
		const arch_register_t *reg  = get_in_reg(node, pos);
		emit_register(reg, mode);
	}
}

/**
 * Returns the target block for a control flow node.
 */
static ir_node *get_cfop_target_block(const ir_node *irn)
{
	assert(get_irn_mode(irn) == mode_X);
	return (ir_node*)get_irn_link(irn);
}

/**
 * Emits the target label for a control flow node.
 */
static void ia32_emit_cfop_target(const ir_node *node)
{
	ir_node *block = get_cfop_target_block(node);
	be_gas_emit_block_name(block);
}

/*
 * positive conditions for signed compares
 */
static const char *const cmp2condition_s[] = {
	NULL, /* always false */
	"e",  /* == */
	"l",  /* <  */
	"le", /* <= */
	"g",  /* >  */
	"ge", /* >= */
	"ne", /* != */
	NULL  /* always true */
};

/*
 * positive conditions for unsigned compares
 */
static const char *const cmp2condition_u[] = {
	NULL, /* always false */
	"e",  /* == */
	"b",  /* <  */
	"be", /* <= */
	"a",  /* >  */
	"ae", /* >= */
	"ne", /* != */
	NULL  /* always true */
};

/**
 * Emit the suffix for a compare instruction.
 */
static void ia32_emit_cmp_suffix(int pnc)
{
	const char *str;

	if (pnc == ia32_pn_Cmp_parity) {
		be_emit_char('p');
		return;
	}

	if (pnc & ia32_pn_Cmp_float || pnc & ia32_pn_Cmp_unsigned) {
		str = cmp2condition_u[pnc & 7];
	} else {
		str = cmp2condition_s[pnc & 7];
	}

	be_emit_string(str);
}

typedef enum ia32_emit_mod_t {
	EMIT_NONE         = 0,
	EMIT_RESPECT_LS   = 1U << 0,
	EMIT_ALTERNATE_AM = 1U << 1,
	EMIT_LONG         = 1U << 2,
	EMIT_HIGH_REG     = 1U << 3,
	EMIT_LOW_REG      = 1U << 4
} ia32_emit_mod_t;
ENUM_BITSET(ia32_emit_mod_t)

/**
 * Emits address mode.
 */
void ia32_emit_am(const ir_node *node)
{
	ir_entity *ent       = get_ia32_am_sc(node);
	int        offs      = get_ia32_am_offs_int(node);
	ir_node   *base      = get_irn_n(node, n_ia32_base);
	int        has_base  = !is_ia32_NoReg_GP(base);
	ir_node   *index     = get_irn_n(node, n_ia32_index);
	int        has_index = !is_ia32_NoReg_GP(index);

	/* just to be sure... */
	assert(!is_ia32_use_frame(node) || get_ia32_frame_ent(node) != NULL);

	/* emit offset */
	if (ent != NULL) {
		const ia32_attr_t *attr = get_ia32_attr_const(node);
		if (is_ia32_am_sc_sign(node))
			be_emit_char('-');
		ia32_emit_entity(ent, attr->data.am_sc_no_pic_adjust);
	}

	/* also handle special case if nothing is set */
	if (offs != 0 || (ent == NULL && !has_base && !has_index)) {
		if (ent != NULL) {
			be_emit_irprintf("%+d", offs);
		} else {
			be_emit_irprintf("%d", offs);
		}
	}

	if (has_base || has_index) {
		be_emit_char('(');

		/* emit base */
		if (has_base) {
			const arch_register_t *reg = get_in_reg(node, n_ia32_base);
			emit_register(reg, NULL);
		}

		/* emit index + scale */
		if (has_index) {
			const arch_register_t *reg = get_in_reg(node, n_ia32_index);
			int scale;
			be_emit_char(',');
			emit_register(reg, NULL);

			scale = get_ia32_am_scale(node);
			if (scale > 0) {
				be_emit_irprintf(",%d", 1 << scale);
			}
		}
		be_emit_char(')');
	}
}

/**
 * fmt  parameter               output
 * ---- ----------------------  ---------------------------------------------
 * %%                           %
 * %AM  <node>                  address mode of the node
 * %AR  const arch_register_t*  address mode of the node or register
 * %ASx <node>                  address mode of the node or source register x
 * %Dx  <node>                  destination register x
 * %I   <node>                  immediate of the node
 * %L   <node>                  control flow target of the node
 * %M   <node>                  mode suffix of the node
 * %P   int                     condition code
 * %R   const arch_register_t*  register
 * %Sx  <node>                  source register x
 * %s   const char*             string
 * %u   unsigned int            unsigned int
 * %d   signed int              signed int
 *
 * x starts at 0
 * # modifier for %ASx, %D, %R, and %S uses ls mode of node to alter register width
 * * modifier does not prefix immediates with $, but AM with *
 * l modifier for %lu and %ld
 * > modifier to output high 8bit register (ah, bh)
 * < modifier to output low 8bit register (al, bl)
 */
static void ia32_emitf(const ir_node *node, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);

	for (;;) {
		const char      *start = fmt;
		ia32_emit_mod_t  mod   = EMIT_NONE;

		while (*fmt != '%' && *fmt != '\n' && *fmt != '\0')
			++fmt;
		if (fmt != start) {
			be_emit_string_len(start, fmt - start);
		}

		if (*fmt == '\n') {
			be_emit_finish_line_gas(node);
			++fmt;
			if (*fmt == '\0')
				break;
			continue;
		}

		if (*fmt == '\0')
			break;

		++fmt;
		for (;;) {
			switch (*fmt) {
			case '*': mod |= EMIT_ALTERNATE_AM; break;
			case '#': mod |= EMIT_RESPECT_LS;   break;
			case 'l': mod |= EMIT_LONG;         break;
			case '>': mod |= EMIT_HIGH_REG;     break;
			case '<': mod |= EMIT_LOW_REG;      break;
			default:
				goto end_of_mods;
			}
			++fmt;
		}
end_of_mods:

		switch (*fmt++) {
			case '%':
				be_emit_char('%');
				break;

			case 'A': {
				switch (*fmt++) {
emit_AM:
					case 'M':
						if (mod & EMIT_ALTERNATE_AM)
							be_emit_char('*');
						ia32_emit_am(node);
						break;

					case 'R': {
						const arch_register_t *reg = va_arg(ap, const arch_register_t*);
						if (get_ia32_op_type(node) == ia32_AddrModeS) {
							goto emit_AM;
						} else {
							if (mod & EMIT_ALTERNATE_AM)
								be_emit_char('*');
							emit_register(reg, NULL);
						}
						break;
					}

					case 'S':
						if (get_ia32_op_type(node) == ia32_AddrModeS) {
							++fmt;
							goto emit_AM;
						} else {
							assert(get_ia32_op_type(node) == ia32_Normal);
							goto emit_S;
						}
						break;

					default: goto unknown;
				}
				break;
			}

			case 'D': {
				unsigned               pos;
				const arch_register_t *reg;

				if (*fmt < '0' || '9' <= *fmt)
					goto unknown;

				pos = *fmt++ - '0';
				reg = get_out_reg(node, pos);
				emit_register(reg, mod & EMIT_RESPECT_LS ? get_ia32_ls_mode(node) : NULL);
				break;
			}

			case 'I':
				if (!(mod & EMIT_ALTERNATE_AM))
					be_emit_char('$');
				emit_ia32_Immediate_no_prefix(node);
				break;

			case 'L':
				ia32_emit_cfop_target(node);
				break;

			case 'M': {
				ia32_emit_mode_suffix_mode(get_ia32_ls_mode(node));
				break;
			}

			case 'P': {
				int pnc = va_arg(ap, int);
				ia32_emit_cmp_suffix(pnc);
				break;
			}

			case 'R': {
				const arch_register_t *reg = va_arg(ap, const arch_register_t*);
				if (mod & EMIT_HIGH_REG) {
					emit_8bit_register_high(reg);
				} else if (mod & EMIT_LOW_REG) {
					emit_8bit_register(reg);
				} else {
					emit_register(reg, mod & EMIT_RESPECT_LS ? get_ia32_ls_mode(node) : NULL);
				}
				break;
			}

emit_S:
			case 'S': {
				unsigned       pos;
				const ir_node *in;

				if (*fmt < '0' || '9' <= *fmt)
					goto unknown;

				pos = *fmt++ - '0';
				in  = get_irn_n(node, pos);
				if (is_ia32_Immediate(in)) {
					if (!(mod & EMIT_ALTERNATE_AM))
						be_emit_char('$');
					emit_ia32_Immediate_no_prefix(in);
				} else {
					const arch_register_t *reg;

					if (mod & EMIT_ALTERNATE_AM)
						be_emit_char('*');
					reg = get_in_reg(node, pos);
					emit_register(reg, mod & EMIT_RESPECT_LS ? get_ia32_ls_mode(node) : NULL);
				}
				break;
			}

			case 's': {
				const char *str = va_arg(ap, const char*);
				be_emit_string(str);
				break;
			}

			case 'u':
				if (mod & EMIT_LONG) {
					unsigned long num = va_arg(ap, unsigned long);
					be_emit_irprintf("%lu", num);
				} else {
					unsigned num = va_arg(ap, unsigned);
					be_emit_irprintf("%u", num);
				}
				break;

			case 'd':
				if (mod & EMIT_LONG) {
					long num = va_arg(ap, long);
					be_emit_irprintf("%ld", num);
				} else {
					int num = va_arg(ap, int);
					be_emit_irprintf("%d", num);
				}
				break;

			default:
unknown:
				panic("unknown format conversion in ia32_emitf()");
		}
	}

	va_end(ap);
}

/**
 * Emits registers and/or address mode of a binary operation.
 */
void ia32_emit_binop(const ir_node *node)
{
	if (is_ia32_Immediate(get_irn_n(node, n_ia32_binary_right))) {
		ia32_emitf(node, "%#S4, %#AS3");
	} else {
		ia32_emitf(node, "%#AS4, %#S3");
	}
}

/**
 * Emits registers and/or address mode of a binary operation.
 */
void ia32_emit_x87_binop(const ir_node *node)
{
	switch (get_ia32_op_type(node)) {
		case ia32_Normal:
			{
				const ia32_x87_attr_t *x87_attr = get_ia32_x87_attr_const(node);
				const arch_register_t *in1      = x87_attr->x87[0];
				const arch_register_t *in       = x87_attr->x87[1];
				const arch_register_t *out      = x87_attr->x87[2];

				if (out == NULL) {
					out = in1;
				} else if (out == in) {
					in = in1;
				}

				be_emit_char('%');
				be_emit_string(arch_register_get_name(in));
				be_emit_cstring(", %");
				be_emit_string(arch_register_get_name(out));
			}
			break;
		case ia32_AddrModeS:
			ia32_emit_am(node);
			break;
		case ia32_AddrModeD:
		default:
			assert(0 && "unsupported op type");
	}
}

/**
 * Emits registers and/or address mode of a unary operation.
 */
void ia32_emit_unop(const ir_node *node, int pos)
{
	char fmt[] = "%ASx";
	fmt[3] = '0' + pos;
	ia32_emitf(node, fmt);
}

static void emit_ia32_IMul(const ir_node *node)
{
	ir_node               *left    = get_irn_n(node, n_ia32_IMul_left);
	const arch_register_t *out_reg = get_out_reg(node, pn_ia32_IMul_res);

	/* do we need the 3-address form? */
	if (is_ia32_NoReg_GP(left) ||
			get_in_reg(node, n_ia32_IMul_left) != out_reg) {
		ia32_emitf(node, "\timul%M %#S4, %#AS3, %#D0\n");
	} else {
		ia32_emitf(node, "\timul%M %#AS4, %#S3\n");
	}
}

/**
 * walks up a tree of copies/perms/spills/reloads to find the original value
 * that is moved around
 */
static ir_node *find_original_value(ir_node *node)
{
	if (irn_visited(node))
		return NULL;

	mark_irn_visited(node);
	if (be_is_Copy(node)) {
		return find_original_value(be_get_Copy_op(node));
	} else if (be_is_CopyKeep(node)) {
		return find_original_value(be_get_CopyKeep_op(node));
	} else if (is_Proj(node)) {
		ir_node *pred = get_Proj_pred(node);
		if (be_is_Perm(pred)) {
			return find_original_value(get_irn_n(pred, get_Proj_proj(node)));
		} else if (be_is_MemPerm(pred)) {
			return find_original_value(get_irn_n(pred, get_Proj_proj(node) + 1));
		} else if (is_ia32_Load(pred)) {
			return find_original_value(get_irn_n(pred, n_ia32_Load_mem));
		} else {
			return node;
		}
	} else if (is_ia32_Store(node)) {
		return find_original_value(get_irn_n(node, n_ia32_Store_val));
	} else if (is_Phi(node)) {
		int i, arity;
		arity = get_irn_arity(node);
		for (i = 0; i < arity; ++i) {
			ir_node *in  = get_irn_n(node, i);
			ir_node *res = find_original_value(in);

			if (res != NULL)
				return res;
		}
		return NULL;
	} else {
		return node;
	}
}

static int determine_final_pnc(const ir_node *node, int flags_pos, int pnc)
{
	ir_node           *flags = get_irn_n(node, flags_pos);
	const ia32_attr_t *flags_attr;
	flags = skip_Proj(flags);

	if (is_ia32_Sahf(flags)) {
		ir_node *cmp = get_irn_n(flags, n_ia32_Sahf_val);
		if (!(is_ia32_FucomFnstsw(cmp) || is_ia32_FucompFnstsw(cmp)
				|| is_ia32_FucomppFnstsw(cmp) || is_ia32_FtstFnstsw(cmp))) {
			inc_irg_visited(current_ir_graph);
			cmp = find_original_value(cmp);
			assert(cmp != NULL);
			assert(is_ia32_FucomFnstsw(cmp) || is_ia32_FucompFnstsw(cmp)
			       || is_ia32_FucomppFnstsw(cmp) || is_ia32_FtstFnstsw(cmp));
		}

		flags_attr = get_ia32_attr_const(cmp);
		if (flags_attr->data.ins_permuted)
			pnc = get_mirrored_pnc(pnc);
		pnc |= ia32_pn_Cmp_float;
	} else if (is_ia32_Ucomi(flags) || is_ia32_Fucomi(flags)
			|| is_ia32_Fucompi(flags)) {
		flags_attr = get_ia32_attr_const(flags);

		if (flags_attr->data.ins_permuted)
			pnc = get_mirrored_pnc(pnc);
		pnc |= ia32_pn_Cmp_float;
	} else {
		flags_attr = get_ia32_attr_const(flags);

		if (flags_attr->data.ins_permuted)
			pnc = get_mirrored_pnc(pnc);
		if (flags_attr->data.cmp_unsigned)
			pnc |= ia32_pn_Cmp_unsigned;
	}

	return pnc;
}

static int ia32_get_negated_pnc(int pnc)
{
	ir_mode *mode = pnc & ia32_pn_Cmp_float ? mode_F : mode_Iu;
	return get_negated_pnc(pnc, mode);
}

void ia32_emit_cmp_suffix_node(const ir_node *node, int flags_pos)
{
	int pnc = get_ia32_condcode(node);
	pnc = determine_final_pnc(node, flags_pos, pnc);

	ia32_emit_cmp_suffix(pnc);
}

/**
 * Emits an exception label for a given node.
 */
static void ia32_emit_exc_label(const ir_node *node)
{
	be_emit_string(be_gas_insn_label_prefix());
	be_emit_irprintf("%lu", get_ia32_exc_label_id(node));
}

/**
 * Returns the Proj with projection number proj and NOT mode_M
 */
static ir_node *get_proj(const ir_node *node, long proj)
{
	const ir_edge_t *edge;
	ir_node         *src;

	assert(get_irn_mode(node) == mode_T && "expected mode_T node");

	foreach_out_edge(node, edge) {
		src = get_edge_src_irn(edge);

		assert(is_Proj(src) && "Proj expected");
		if (get_irn_mode(src) == mode_M)
			continue;

		if (get_Proj_proj(src) == proj)
			return src;
	}
	return NULL;
}

static int can_be_fallthrough(const ir_node *node)
{
	ir_node *target_block = get_cfop_target_block(node);
	ir_node *block        = get_nodes_block(node);
	return get_prev_block_sched(target_block) == block;
}

/**
 * Emits the jump sequence for a conditional jump (cmp + jmp_true + jmp_false)
 */
static void emit_ia32_Jcc(const ir_node *node)
{
	int            need_parity_label = 0;
	const ir_node *proj_true;
	const ir_node *proj_false;
	int            pnc = get_ia32_condcode(node);

	pnc = determine_final_pnc(node, 0, pnc);

	/* get both Projs */
	proj_true = get_proj(node, pn_ia32_Jcc_true);
	assert(proj_true && "Jcc without true Proj");

	proj_false = get_proj(node, pn_ia32_Jcc_false);
	assert(proj_false && "Jcc without false Proj");

	if (can_be_fallthrough(proj_true)) {
		/* exchange both proj's so the second one can be omitted */
		const ir_node *t = proj_true;

		proj_true  = proj_false;
		proj_false = t;
		pnc        = ia32_get_negated_pnc(pnc);
	}

	if (pnc & ia32_pn_Cmp_float) {
		/* Some floating point comparisons require a test of the parity flag,
		 * which indicates that the result is unordered */
		switch (pnc & 0x0f) {
		case pn_Cmp_Uo: {
			ia32_emitf(proj_true, "\tjp %L\n");
			break;
		}

		case pn_Cmp_Leg:
			ia32_emitf(proj_true, "\tjnp %L\n");
			break;

		case pn_Cmp_Eq:
		case pn_Cmp_Lt:
		case pn_Cmp_Le:
			/* we need a local label if the false proj is a fallthrough
			 * as the falseblock might have no label emitted then */
			if (can_be_fallthrough(proj_false)) {
				need_parity_label = 1;
				ia32_emitf(proj_false, "\tjp 1f\n");
			} else {
				ia32_emitf(proj_false, "\tjp %L\n");
			}
			goto emit_jcc;

		case pn_Cmp_Ug:
		case pn_Cmp_Uge:
		case pn_Cmp_Ne:
			ia32_emitf(proj_true, "\tjp %L\n");
			goto emit_jcc;

		default:
			goto emit_jcc;
		}
	} else {
emit_jcc:
		ia32_emitf(proj_true, "\tj%P %L\n", pnc);
	}

	if (need_parity_label) {
		ia32_emitf(NULL, "1:\n");
	}

	/* the second Proj might be a fallthrough */
	if (can_be_fallthrough(proj_false)) {
		ia32_emitf(proj_false, "\t/* fallthrough to %L */\n");
	} else {
		ia32_emitf(proj_false, "\tjmp %L\n");
	}
}

/**
 * Emits an ia32 Setcc. This is mostly easy but some floating point compares
 * are tricky.
 */
static void emit_ia32_Setcc(const ir_node *node)
{
	const arch_register_t *dreg = get_out_reg(node, pn_ia32_Setcc_res);

	int pnc = get_ia32_condcode(node);
	pnc     = determine_final_pnc(node, n_ia32_Setcc_eflags, pnc);
	if (pnc & ia32_pn_Cmp_float) {
		switch (pnc & 0x0f) {
		case pn_Cmp_Uo:
			ia32_emitf(node, "\tsetp %#R\n", dreg);
			return;

		case pn_Cmp_Leg:
			ia32_emitf(node, "\tsetnp %#R\n", dreg);
			return;

		case pn_Cmp_Eq:
		case pn_Cmp_Lt:
		case pn_Cmp_Le:
			ia32_emitf(node, "\tset%P %<R\n", pnc, dreg);
			ia32_emitf(node, "\tsetnp %>R\n", dreg);
			ia32_emitf(node, "\tandb %>R, %<R\n", dreg, dreg);
			return;

		case pn_Cmp_Ug:
		case pn_Cmp_Uge:
		case pn_Cmp_Ne:
			ia32_emitf(node, "\tset%P %<R\n", pnc, dreg);
			ia32_emitf(node, "\tsetp %>R\n", dreg);
			ia32_emitf(node, "\torb %>R, %<R\n", dreg, dreg);
			return;

		default:
			break;
		}
	}
	ia32_emitf(node, "\tset%P %#R\n", pnc, dreg);
}

static void emit_ia32_CMovcc(const ir_node *node)
{
	const ia32_attr_t     *attr = get_ia32_attr_const(node);
	const arch_register_t *out  = arch_irn_get_register(node, pn_ia32_res);
	int                    pnc  = get_ia32_condcode(node);
	const arch_register_t *in_true;
	const arch_register_t *in_false;

	pnc = determine_final_pnc(node, n_ia32_CMovcc_eflags, pnc);
	/* although you can't set ins_permuted in the constructor it might still
	 * be set by memory operand folding
	 * Permuting inputs of a cmov means the condition is negated!
	 */
	if (attr->data.ins_permuted)
		pnc = ia32_get_negated_pnc(pnc);

	in_true  = arch_get_irn_register(get_irn_n(node, n_ia32_CMovcc_val_true));
	in_false = arch_get_irn_register(get_irn_n(node, n_ia32_CMovcc_val_false));

	/* should be same constraint fullfilled? */
	if (out == in_false) {
		/* yes -> nothing to do */
	} else if (out == in_true) {
		const arch_register_t *tmp;

		assert(get_ia32_op_type(node) == ia32_Normal);

		pnc = ia32_get_negated_pnc(pnc);

		tmp      = in_true;
		in_true  = in_false;
		in_false = tmp;
	} else {
		/* we need a mov */
		ia32_emitf(node, "\tmovl %R, %R\n", in_false, out);
	}

	/* TODO: handling of Nans isn't correct yet */
	if (pnc & ia32_pn_Cmp_float) {
		switch (pnc & 0x0f) {
		case pn_Cmp_Uo:
		case pn_Cmp_Leg:
		case pn_Cmp_Eq:
		case pn_Cmp_Lt:
		case pn_Cmp_Le:
		case pn_Cmp_Ug:
		case pn_Cmp_Uge:
		case pn_Cmp_Ne:
			panic("CMov with floatingpoint compare/parity not supported yet");
		}
	}

	ia32_emitf(node, "\tcmov%P %#AR, %#R\n", pnc, in_true, out);
}


/* jump table entry (target and corresponding number) */
typedef struct branch_t {
	ir_node *target;
	int      value;
} branch_t;

/* jump table for switch generation */
typedef struct jmp_tbl_t {
	ir_node  *defProj;                 /**< default target */
	long      min_value;               /**< smallest switch case */
	long      max_value;               /**< largest switch case */
	long      num_branches;            /**< number of jumps */
	char      label[SNPRINTF_BUF_LEN]; /**< label of the jump table */
	branch_t *branches;                /**< jump array */
} jmp_tbl_t;

/**
 * Compare two variables of type branch_t. Used to sort all switch cases
 */
static int ia32_cmp_branch_t(const void *a, const void *b)
{
	branch_t *b1 = (branch_t *)a;
	branch_t *b2 = (branch_t *)b;

	if (b1->value <= b2->value)
		return -1;
	else
		return 1;
}

static void generate_jump_table(jmp_tbl_t *tbl, const ir_node *node)
{
	int                 i;
	long                pnc;
	long                default_pn;
	ir_node            *proj;
	const ir_edge_t    *edge;

	/* fill the table structure */
	get_unique_label(tbl->label, SNPRINTF_BUF_LEN, "TBL_");
	tbl->defProj      = NULL;
	tbl->num_branches = get_irn_n_edges(node) - 1;
	tbl->branches     = XMALLOCNZ(branch_t, tbl->num_branches);
	tbl->min_value    = LONG_MAX;
	tbl->max_value    = LONG_MIN;

	default_pn = get_ia32_condcode(node);
	i = 0;
	/* go over all proj's and collect them */
	foreach_out_edge(node, edge) {
		proj = get_edge_src_irn(edge);
		assert(is_Proj(proj) && "Only proj allowed at SwitchJmp");

		pnc = get_Proj_proj(proj);

		/* check for default proj */
		if (pnc == default_pn) {
			assert(tbl->defProj == NULL && "found two default Projs at SwitchJmp");
			tbl->defProj = proj;
		} else {
			tbl->min_value = pnc < tbl->min_value ? pnc : tbl->min_value;
			tbl->max_value = pnc > tbl->max_value ? pnc : tbl->max_value;

			/* create branch entry */
			tbl->branches[i].target = proj;
			tbl->branches[i].value  = pnc;
			++i;
		}

	}
	assert(i == tbl->num_branches);

	/* sort the branches by their number */
	qsort(tbl->branches, tbl->num_branches, sizeof(tbl->branches[0]), ia32_cmp_branch_t);
}

/**
 * Emits code for a SwitchJmp (creates a jump table if
 * possible otherwise a cmp-jmp cascade). Port from
 * cggg ia32 backend
 */
static void emit_ia32_SwitchJmp(const ir_node *node)
{
	unsigned long       interval;
	int                 last_value, i;
	jmp_tbl_t           tbl;

	/* fill the table structure */
	generate_jump_table(&tbl, node);

	/* two-complement's magic make this work without overflow */
	interval = tbl.max_value - tbl.min_value;

	/* emit the table */
	ia32_emitf(node,        "\tcmpl $%u, %S0\n", interval);
	ia32_emitf(tbl.defProj, "\tja %L\n");

	if (tbl.num_branches > 1) {
		/* create table */
		ia32_emitf(node, "\tjmp *%s(,%S0,4)\n", tbl.label);

		be_gas_emit_switch_section(GAS_SECTION_RODATA);
		ia32_emitf(NULL, "\t.align 4\n");
		ia32_emitf(NULL, "%s:\n", tbl.label);

		last_value = tbl.branches[0].value;
		for (i = 0; i != tbl.num_branches; ++i) {
			while (last_value != tbl.branches[i].value) {
				ia32_emitf(tbl.defProj, ".long %L\n");
				++last_value;
			}
			ia32_emitf(tbl.branches[i].target, ".long %L\n");
			++last_value;
		}
		be_gas_emit_switch_section(GAS_SECTION_TEXT);
	} else {
		/* one jump is enough */
		ia32_emitf(tbl.branches[0].target, "\tjmp %L\n");
	}

	free(tbl.branches);
}

/**
 * Emits code for a unconditional jump.
 */
static void emit_ia32_Jmp(const ir_node *node)
{
	ir_node *block;

	/* for now, the code works for scheduled and non-schedules blocks */
	block = get_nodes_block(node);

	/* we have a block schedule */
	if (can_be_fallthrough(node)) {
		ia32_emitf(node, "\t/* fallthrough to %L */\n");
	} else {
		ia32_emitf(node, "\tjmp %L\n");
	}
}

/**
 * Emit an inline assembler operand.
 *
 * @param node  the ia32_ASM node
 * @param s     points to the operand (a %c)
 *
 * @return  pointer to the first char in s NOT in the current operand
 */
static const char* emit_asm_operand(const ir_node *node, const char *s)
{
	const ia32_attr_t     *ia32_attr = get_ia32_attr_const(node);
	const ia32_asm_attr_t *attr      = CONST_CAST_IA32_ATTR(ia32_asm_attr_t,
                                                            ia32_attr);
	const arch_register_t *reg;
	const ia32_asm_reg_t  *asm_regs = attr->register_map;
	const ia32_asm_reg_t  *asm_reg;
	const char            *reg_name;
	char                   c;
	char                   modifier = 0;
	int                    num      = -1;
	int                    p;

	assert(*s == '%');
	c = *(++s);

	/* parse modifiers */
	switch (c) {
	case 0:
		ir_fprintf(stderr, "Warning: asm text (%+F) ends with %%\n", node);
		be_emit_char('%');
		return s + 1;
	case '%':
		be_emit_char('%');
		return s + 1;
	case 'w':
	case 'b':
	case 'h':
		modifier = c;
		++s;
		break;
	case '0':
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9':
		break;
	default:
		ir_fprintf(stderr,
				"Warning: asm text (%+F) contains unknown modifier '%c' for asm op\n",
				node, c);
		++s;
		break;
	}

	/* parse number */
	sscanf(s, "%d%n", &num, &p);
	if (num < 0) {
		ir_fprintf(stderr, "Warning: Couldn't parse assembler operand (%+F)\n",
		           node);
		return s;
	} else {
		s += p;
	}

	if (num < 0 || ARR_LEN(asm_regs) <= num) {
		ir_fprintf(stderr,
				"Error: Custom assembler references invalid input/output (%+F)\n",
				node);
		return s;
	}
	asm_reg = & asm_regs[num];
	assert(asm_reg->valid);

	/* get register */
	if (asm_reg->use_input == 0) {
		reg = get_out_reg(node, asm_reg->inout_pos);
	} else {
		ir_node *pred = get_irn_n(node, asm_reg->inout_pos);

		/* might be an immediate value */
		if (is_ia32_Immediate(pred)) {
			emit_ia32_Immediate(pred);
			return s;
		}
		reg = get_in_reg(node, asm_reg->inout_pos);
	}
	if (reg == NULL) {
		ir_fprintf(stderr,
				"Warning: no register assigned for %d asm op (%+F)\n",
				num, node);
		return s;
	}

	if (asm_reg->memory) {
		be_emit_char('(');
	}

	/* emit it */
	if (modifier != 0) {
		be_emit_char('%');
		switch (modifier) {
		case 'b':
			reg_name = ia32_get_mapped_reg_name(isa->regs_8bit, reg);
			break;
		case 'h':
			reg_name = ia32_get_mapped_reg_name(isa->regs_8bit_high, reg);
			break;
		case 'w':
			reg_name = ia32_get_mapped_reg_name(isa->regs_16bit, reg);
			break;
		default:
			panic("Invalid asm op modifier");
		}
		be_emit_string(reg_name);
	} else {
		emit_register(reg, asm_reg->mode);
	}

	if (asm_reg->memory) {
		be_emit_char(')');
	}

	return s;
}

/**
 * Emits code for an ASM pseudo op.
 */
static void emit_ia32_Asm(const ir_node *node)
{
	const void            *gen_attr = get_irn_generic_attr_const(node);
	const ia32_asm_attr_t *attr
		= CONST_CAST_IA32_ATTR(ia32_asm_attr_t, gen_attr);
	ident                 *asm_text = attr->asm_text;
	const char            *s        = get_id_str(asm_text);

	ia32_emitf(node, "#APP\t\n");

	if (s[0] != '\t')
		be_emit_char('\t');

	while (*s != 0) {
		if (*s == '%') {
			s = emit_asm_operand(node, s);
		} else {
			be_emit_char(*s++);
		}
	}

	ia32_emitf(NULL, "\n#NO_APP\n");
}


/**
 * Emit movsb/w instructions to make mov count divideable by 4
 */
static void emit_CopyB_prolog(unsigned size)
{
	if (size & 1)
		ia32_emitf(NULL, "\tmovsb\n");
	if (size & 2)
		ia32_emitf(NULL, "\tmovsw\n");
}

/**
 * Emit rep movsd instruction for memcopy.
 */
static void emit_ia32_CopyB(const ir_node *node)
{
	unsigned size = get_ia32_copyb_size(node);

	emit_CopyB_prolog(size);
	ia32_emitf(node, "\trep movsd\n");
}

/**
 * Emits unrolled memcopy.
 */
static void emit_ia32_CopyB_i(const ir_node *node)
{
	unsigned size = get_ia32_copyb_size(node);

	emit_CopyB_prolog(size);

	size >>= 2;
	while (size--) {
		ia32_emitf(NULL, "\tmovsd\n");
	}
}


/**
 * Emit code for conversions (I, FP), (FP, I) and (FP, FP).
 */
static void emit_ia32_Conv_with_FP(const ir_node *node, const char* conv_f,
		const char* conv_d)
{
	ir_mode            *ls_mode = get_ia32_ls_mode(node);
	int                 ls_bits = get_mode_size_bits(ls_mode);
	const char         *conv    = ls_bits == 32 ? conv_f : conv_d;

	ia32_emitf(node, "\tcvt%s %AS3, %D0\n", conv);
}

static void emit_ia32_Conv_I2FP(const ir_node *node)
{
	emit_ia32_Conv_with_FP(node, "si2ss", "si2sd");
}

static void emit_ia32_Conv_FP2I(const ir_node *node)
{
	emit_ia32_Conv_with_FP(node, "ss2si", "sd2si");
}

static void emit_ia32_Conv_FP2FP(const ir_node *node)
{
	emit_ia32_Conv_with_FP(node, "sd2ss", "ss2sd");
}

/**
 * Emits code for an Int conversion.
 */
static void emit_ia32_Conv_I2I(const ir_node *node)
{
	ir_mode *smaller_mode = get_ia32_ls_mode(node);
	int      signed_mode  = mode_is_signed(smaller_mode);
	const char *sign_suffix;

	assert(!mode_is_float(smaller_mode));

	sign_suffix = signed_mode ? "s" : "z";
	ia32_emitf(node, "\tmov%s%Ml %#AS3, %D0\n", sign_suffix);
}

/**
 * Emits a call
 */
static void emit_ia32_Call(const ir_node *node)
{
	/* Special case: Call must not have its immediates prefixed by $, instead
	 * address mode is prefixed by *. */
	ia32_emitf(node, "\tcall %*AS3\n");
}


/**
 * Emits code to increase stack pointer.
 */
static void emit_be_IncSP(const ir_node *node)
{
	int offs = be_get_IncSP_offset(node);

	if (offs == 0)
		return;

	if (offs > 0) {
		ia32_emitf(node, "\tsubl $%u, %D0\n", offs);
	} else {
		ia32_emitf(node, "\taddl $%u, %D0\n", -offs);
	}
}

/**
 * Emits code for Copy/CopyKeep.
 */
static void Copy_emitter(const ir_node *node, const ir_node *op)
{
	const arch_register_t *in  = arch_get_irn_register(op);
	const arch_register_t *out = arch_get_irn_register(node);

	if (in == out) {
		return;
	}
	/* copies of vf nodes aren't real... */
	if (arch_register_get_class(in) == &ia32_reg_classes[CLASS_ia32_vfp])
		return;

	if (get_irn_mode(node) == mode_E) {
		ia32_emitf(node, "\tmovsd %R, %R\n", in, out);
	} else {
		ia32_emitf(node, "\tmovl %R, %R\n", in, out);
	}
}

static void emit_be_Copy(const ir_node *node)
{
	Copy_emitter(node, be_get_Copy_op(node));
}

static void emit_be_CopyKeep(const ir_node *node)
{
	Copy_emitter(node, be_get_CopyKeep_op(node));
}

/**
 * Emits code for exchange.
 */
static void emit_be_Perm(const ir_node *node)
{
	const arch_register_t *in0, *in1;
	const arch_register_class_t *cls0, *cls1;

	in0 = arch_get_irn_register(get_irn_n(node, 0));
	in1 = arch_get_irn_register(get_irn_n(node, 1));

	cls0 = arch_register_get_class(in0);
	cls1 = arch_register_get_class(in1);

	assert(cls0 == cls1 && "Register class mismatch at Perm");

	if (cls0 == &ia32_reg_classes[CLASS_ia32_gp]) {
		ia32_emitf(node, "\txchg %R, %R\n", in1, in0);
	} else if (cls0 == &ia32_reg_classes[CLASS_ia32_xmm]) {
		ia32_emitf(NULL, "\txorpd %R, %R\n", in1, in0);
		ia32_emitf(NULL, "\txorpd %R, %R\n", in0, in1);
		ia32_emitf(node, "\txorpd %R, %R\n", in1, in0);
	} else if (cls0 == &ia32_reg_classes[CLASS_ia32_vfp]) {
		/* is a NOP */
	} else if (cls0 == &ia32_reg_classes[CLASS_ia32_st]) {
		/* is a NOP */
	} else {
		panic("unexpected register class in be_Perm (%+F)", node);
	}
}

/**
 * Emits code for Constant loading.
 */
static void emit_ia32_Const(const ir_node *node)
{
	ia32_emitf(node, "\tmovl %I, %D0\n");
}

/**
 * Emits code to load the TLS base
 */
static void emit_ia32_LdTls(const ir_node *node)
{
	ia32_emitf(node, "\tmovl %%gs:0, %D0\n");
}

/* helper function for emit_ia32_Minus64Bit */
static void emit_mov(const ir_node* node, const arch_register_t *src, const arch_register_t *dst)
{
	ia32_emitf(node, "\tmovl %R, %R\n", src, dst);
}

/* helper function for emit_ia32_Minus64Bit */
static void emit_neg(const ir_node* node, const arch_register_t *reg)
{
	ia32_emitf(node, "\tnegl %R\n", reg);
}

/* helper function for emit_ia32_Minus64Bit */
static void emit_sbb0(const ir_node* node, const arch_register_t *reg)
{
	ia32_emitf(node, "\tsbbl $0, %R\n", reg);
}

/* helper function for emit_ia32_Minus64Bit */
static void emit_sbb(const ir_node* node, const arch_register_t *src, const arch_register_t *dst)
{
	ia32_emitf(node, "\tsbbl %R, %R\n", src, dst);
}

/* helper function for emit_ia32_Minus64Bit */
static void emit_xchg(const ir_node* node, const arch_register_t *src, const arch_register_t *dst)
{
	ia32_emitf(node, "\txchgl %R, %R\n", src, dst);
}

/* helper function for emit_ia32_Minus64Bit */
static void emit_zero(const ir_node* node, const arch_register_t *reg)
{
	ia32_emitf(node, "\txorl %R, %R\n", reg, reg);
}

static void emit_ia32_Minus64Bit(const ir_node *node)
{
	const arch_register_t *in_lo  = get_in_reg(node, 0);
	const arch_register_t *in_hi  = get_in_reg(node, 1);
	const arch_register_t *out_lo = get_out_reg(node, 0);
	const arch_register_t *out_hi = get_out_reg(node, 1);

	if (out_lo == in_lo) {
		if (out_hi != in_hi) {
			/* a -> a, b -> d */
			goto zero_neg;
		} else {
			/* a -> a, b -> b */
			goto normal_neg;
		}
	} else if (out_lo == in_hi) {
		if (out_hi == in_lo) {
			/* a -> b, b -> a */
			emit_xchg(node, in_lo, in_hi);
			goto normal_neg;
		} else {
			/* a -> b, b -> d */
			emit_mov(node, in_hi, out_hi);
			emit_mov(node, in_lo, out_lo);
			goto normal_neg;
		}
	} else {
		if (out_hi == in_lo) {
			/* a -> c, b -> a */
			emit_mov(node, in_lo, out_lo);
			goto zero_neg;
		} else if (out_hi == in_hi) {
			/* a -> c, b -> b */
			emit_mov(node, in_lo, out_lo);
			goto normal_neg;
		} else {
			/* a -> c, b -> d */
			emit_mov(node, in_lo, out_lo);
			goto zero_neg;
		}
	}

normal_neg:
	emit_neg( node, out_hi);
	emit_neg( node, out_lo);
	emit_sbb0(node, out_hi);
	return;

zero_neg:
	emit_zero(node, out_hi);
	emit_neg( node, out_lo);
	emit_sbb( node, in_hi, out_hi);
}

static void emit_ia32_GetEIP(const ir_node *node)
{
	ia32_emitf(node, "\tcall %s\n", pic_base_label);
	ia32_emitf(NULL, "%s:\n", pic_base_label);
	ia32_emitf(node, "\tpopl %D0\n");
}

static void emit_ia32_ClimbFrame(const ir_node *node)
{
	const ia32_climbframe_attr_t *attr = get_ia32_climbframe_attr_const(node);

	ia32_emitf(node, "\tmovl %S0, %D0\n");
	ia32_emitf(node, "\tmovl $%u, %S1\n", attr->count);
	be_gas_emit_block_name(node);
	be_emit_cstring(":\n");
	be_emit_write_line();
	ia32_emitf(node, "\tmovl (%D0), %D0\n");
	ia32_emitf(node, "\tdec %S1\n");
	be_emit_cstring("\tjnz ");
	be_gas_emit_block_name(node);
	be_emit_finish_line_gas(node);
}

static void emit_be_Return(const ir_node *node)
{
	unsigned pop = be_Return_get_pop(node);

	if (pop > 0 || be_Return_get_emit_pop(node)) {
		ia32_emitf(node, "\tret $%u\n", pop);
	} else {
		ia32_emitf(node, "\tret\n");
	}
}

static void emit_Nothing(const ir_node *node)
{
	(void) node;
}


/**
 * Enters the emitter functions for handled nodes into the generic
 * pointer of an opcode.
 */
static void ia32_register_emitters(void)
{
#define IA32_EMIT2(a,b) op_ia32_##a->ops.generic = (op_func)emit_ia32_##b
#define IA32_EMIT(a)    IA32_EMIT2(a,a)
#define EMIT(a)         op_##a->ops.generic = (op_func)emit_##a
#define IGN(a)          op_##a->ops.generic = (op_func)emit_Nothing
#define BE_EMIT(a)      op_be_##a->ops.generic = (op_func)emit_be_##a
#define BE_IGN(a)       op_be_##a->ops.generic = (op_func)emit_Nothing

	/* first clear the generic function pointer for all ops */
	clear_irp_opcodes_generic_func();

	/* register all emitter functions defined in spec */
	ia32_register_spec_emitters();

	/* other ia32 emitter functions */
	IA32_EMIT2(Conv_I2I8Bit, Conv_I2I);
	IA32_EMIT(Asm);
	IA32_EMIT(CMovcc);
	IA32_EMIT(Call);
	IA32_EMIT(Const);
	IA32_EMIT(Conv_FP2FP);
	IA32_EMIT(Conv_FP2I);
	IA32_EMIT(Conv_I2FP);
	IA32_EMIT(Conv_I2I);
	IA32_EMIT(CopyB);
	IA32_EMIT(CopyB_i);
	IA32_EMIT(GetEIP);
	IA32_EMIT(IMul);
	IA32_EMIT(Jcc);
	IA32_EMIT(Setcc);
	IA32_EMIT(LdTls);
	IA32_EMIT(Minus64Bit);
	IA32_EMIT(SwitchJmp);
	IA32_EMIT(ClimbFrame);
	IA32_EMIT(Jmp);

	/* benode emitter */
	BE_EMIT(Copy);
	BE_EMIT(CopyKeep);
	BE_EMIT(IncSP);
	BE_EMIT(Perm);
	BE_EMIT(Return);

	BE_IGN(Barrier);
	BE_IGN(Keep);
	BE_IGN(Start);

	/* firm emitter */
	IGN(Phi);

#undef BE_EMIT
#undef EMIT
#undef IGN
#undef IA32_EMIT2
#undef IA32_EMIT
}

typedef void (*emit_func_ptr) (const ir_node *);

/**
 * Assign and emit an exception label if the current instruction can fail.
 */
static void ia32_assign_exc_label(ir_node *node)
{
	/* assign a new ID to the instruction */
	set_ia32_exc_label_id(node, ++exc_label_id);
	/* print it */
	ia32_emit_exc_label(node);
	be_emit_char(':');
	be_emit_pad_comment();
	be_emit_cstring("/* exception to Block ");
	ia32_emit_cfop_target(node);
	be_emit_cstring(" */\n");
	be_emit_write_line();
}

/**
 * Emits code for a node.
 */
static void ia32_emit_node(ir_node *node)
{
	ir_op *op = get_irn_op(node);

	DBG((dbg, LEVEL_1, "emitting code for %+F\n", node));

	if (is_ia32_irn(node)) {
		if (get_ia32_exc_label(node)) {
			/* emit the exception label of this instruction */
			ia32_assign_exc_label(node);
		}
		if (mark_spill_reload) {
			if (is_ia32_is_spill(node)) {
				ia32_emitf(NULL, "\txchg %ebx, %ebx        /* spill mark */\n");
			}
			if (is_ia32_is_reload(node)) {
				ia32_emitf(NULL, "\txchg %edx, %edx        /* reload mark */\n");
			}
			if (is_ia32_is_remat(node)) {
				ia32_emitf(NULL, "\txchg %ecx, %ecx        /* remat mark */\n");
			}
		}
	}
	if (op->ops.generic) {
		emit_func_ptr func = (emit_func_ptr) op->ops.generic;

		be_dbg_set_dbg_info(get_irn_dbg_info(node));

		(*func) (node);
	} else {
		emit_Nothing(node);
		ir_fprintf(stderr, "Error: No emit handler for node %+F (%+G, graph %+F)\n", node, node, current_ir_graph);
		abort();
	}
}

/**
 * Emits gas alignment directives
 */
static void ia32_emit_alignment(unsigned align, unsigned skip)
{
	ia32_emitf(NULL, "\t.p2align %u,,%u\n", align, skip);
}

/**
 * Emits gas alignment directives for Labels depended on cpu architecture.
 */
static void ia32_emit_align_label(void)
{
	unsigned align        = ia32_cg_config.label_alignment;
	unsigned maximum_skip = ia32_cg_config.label_alignment_max_skip;
	ia32_emit_alignment(align, maximum_skip);
}

/**
 * Test whether a block should be aligned.
 * For cpus in the P4/Athlon class it is useful to align jump labels to
 * 16 bytes. However we should only do that if the alignment nops before the
 * label aren't executed more often than we have jumps to the label.
 */
static int should_align_block(const ir_node *block)
{
	static const double DELTA = .0001;
	ir_graph     *irg         = get_irn_irg(block);
	ir_exec_freq *exec_freq   = be_get_irg_exec_freq(irg);
	ir_node      *prev        = get_prev_block_sched(block);
	double        block_freq;
	double        prev_freq = 0;  /**< execfreq of the fallthrough block */
	double        jmp_freq  = 0;  /**< execfreq of all non-fallthrough blocks */
	int           i, n_cfgpreds;

	if (exec_freq == NULL)
		return 0;
	if (ia32_cg_config.label_alignment_factor <= 0)
		return 0;

	block_freq = get_block_execfreq(exec_freq, block);
	if (block_freq < DELTA)
		return 0;

	n_cfgpreds = get_Block_n_cfgpreds(block);
	for (i = 0; i < n_cfgpreds; ++i) {
		const ir_node *pred      = get_Block_cfgpred_block(block, i);
		double         pred_freq = get_block_execfreq(exec_freq, pred);

		if (pred == prev) {
			prev_freq += pred_freq;
		} else {
			jmp_freq  += pred_freq;
		}
	}

	if (prev_freq < DELTA && !(jmp_freq < DELTA))
		return 1;

	jmp_freq /= prev_freq;

	return jmp_freq > ia32_cg_config.label_alignment_factor;
}

/**
 * Emit the block header for a block.
 *
 * @param block       the block
 * @param prev_block  the previous block
 */
static void ia32_emit_block_header(ir_node *block)
{
	ir_graph     *irg = current_ir_graph;
	int           need_label = block_needs_label(block);
	int           i, arity;
	ir_exec_freq *exec_freq = be_get_irg_exec_freq(irg);

	if (block == get_irg_end_block(irg))
		return;

	if (ia32_cg_config.label_alignment > 0) {
		/* align the current block if:
		 * a) if should be aligned due to its execution frequency
		 * b) there is no fall-through here
		 */
		if (should_align_block(block)) {
			ia32_emit_align_label();
		} else {
			/* if the predecessor block has no fall-through,
			   we can always align the label. */
			int i;
			int has_fallthrough = 0;

			for (i = get_Block_n_cfgpreds(block) - 1; i >= 0; --i) {
				ir_node *cfg_pred = get_Block_cfgpred(block, i);
				if (can_be_fallthrough(cfg_pred)) {
					has_fallthrough = 1;
					break;
				}
			}

			if (!has_fallthrough)
				ia32_emit_align_label();
		}
	}

	if (need_label) {
		be_gas_emit_block_name(block);
		be_emit_char(':');

		be_emit_pad_comment();
		be_emit_cstring("   /* ");
	} else {
		be_emit_cstring("\t/* ");
		be_gas_emit_block_name(block);
		be_emit_cstring(": ");
	}

	be_emit_cstring("preds:");

	/* emit list of pred blocks in comment */
	arity = get_irn_arity(block);
	if (arity <= 0) {
		be_emit_cstring(" none");
	} else {
		for (i = 0; i < arity; ++i) {
			ir_node *predblock = get_Block_cfgpred_block(block, i);
			be_emit_irprintf(" %d", get_irn_node_nr(predblock));
		}
	}
	if (exec_freq != NULL) {
		be_emit_irprintf(", freq: %f",
		                 get_block_execfreq(exec_freq, block));
	}
	be_emit_cstring(" */\n");
	be_emit_write_line();
}

/**
 * Walks over the nodes in a block connected by scheduling edges
 * and emits code for each node.
 */
static void ia32_gen_block(ir_node *block)
{
	ir_node *node;

	ia32_emit_block_header(block);

	/* emit the contents of the block */
	be_dbg_set_dbg_info(get_irn_dbg_info(block));
	sched_foreach(block, node) {
		ia32_emit_node(node);
	}
}

typedef struct exc_entry {
	ir_node *exc_instr;  /** The instruction that can issue an exception. */
	ir_node *block;      /** The block to call then. */
} exc_entry;

/**
 * Block-walker:
 * Sets labels for control flow nodes (jump target).
 * Links control predecessors to there destination blocks.
 */
static void ia32_gen_labels(ir_node *block, void *data)
{
	exc_entry **exc_list = (exc_entry**)data;
	ir_node *pred;
	int     n;

	for (n = get_Block_n_cfgpreds(block) - 1; n >= 0; --n) {
		pred = get_Block_cfgpred(block, n);
		set_irn_link(pred, block);

		pred = skip_Proj(pred);
		if (is_ia32_irn(pred) && get_ia32_exc_label(pred)) {
			exc_entry e;

			e.exc_instr = pred;
			e.block     = block;
			ARR_APP1(exc_entry, *exc_list, e);
			set_irn_link(pred, block);
		}
	}
}

/**
 * Compare two exception_entries.
 */
static int cmp_exc_entry(const void *a, const void *b)
{
	const exc_entry *ea = (const exc_entry*)a;
	const exc_entry *eb = (const exc_entry*)b;

	if (get_ia32_exc_label_id(ea->exc_instr) < get_ia32_exc_label_id(eb->exc_instr))
		return -1;
	return +1;
}

/**
 * Main driver. Emits the code for one routine.
 */
void ia32_gen_routine(ir_graph *irg)
{
	ir_entity        *entity    = get_irg_entity(irg);
	exc_entry        *exc_list  = NEW_ARR_F(exc_entry, 0);
	const arch_env_t *arch_env  = be_get_irg_arch_env(irg);
	ia32_irg_data_t  *irg_data  = ia32_get_irg_data(irg);
	ir_node         **blk_sched = irg_data->blk_sched;
	int i, n;

	isa      = (ia32_isa_t*) arch_env;
	do_pic   = be_get_irg_options(irg)->pic;

	be_gas_elf_type_char = '@';

	ia32_register_emitters();

	get_unique_label(pic_base_label, sizeof(pic_base_label), "PIC_BASE");

	be_dbg_method_begin(entity);
	be_gas_emit_function_prolog(entity, ia32_cg_config.function_alignment);

	/* we use links to point to target blocks */
	ir_reserve_resources(irg, IR_RESOURCE_IRN_LINK);
	irg_block_walk_graph(irg, ia32_gen_labels, NULL, &exc_list);

	/* initialize next block links */
	n = ARR_LEN(blk_sched);
	for (i = 0; i < n; ++i) {
		ir_node *block = blk_sched[i];
		ir_node *prev  = i > 0 ? blk_sched[i-1] : NULL;

		set_irn_link(block, prev);
	}

	for (i = 0; i < n; ++i) {
		ir_node *block = blk_sched[i];

		ia32_gen_block(block);
	}

	be_gas_emit_function_epilog(entity);
	be_dbg_method_end();
	be_emit_char('\n');
	be_emit_write_line();

	ir_free_resources(irg, IR_RESOURCE_IRN_LINK);

	/* Sort the exception table using the exception label id's.
	   Those are ascending with ascending addresses. */
	qsort(exc_list, ARR_LEN(exc_list), sizeof(exc_list[0]), cmp_exc_entry);
	{
		int i;

		for (i = 0; i < ARR_LEN(exc_list); ++i) {
			be_emit_cstring("\t.long ");
			ia32_emit_exc_label(exc_list[i].exc_instr);
			be_emit_char('\n');
			be_emit_cstring("\t.long ");
			be_gas_emit_block_name(exc_list[i].block);
			be_emit_char('\n');
		}
	}
	DEL_ARR_F(exc_list);
}

static const lc_opt_table_entry_t ia32_emitter_options[] = {
	LC_OPT_ENT_BOOL("mark_spill_reload",   "mark spills and reloads with ud opcodes", &mark_spill_reload),
	LC_OPT_LAST
};

/* ==== Experimental binary emitter ==== */

static unsigned char reg_gp_map[N_ia32_gp_REGS];
//static unsigned char reg_mmx_map[N_ia32_mmx_REGS];
//static unsigned char reg_sse_map[N_ia32_xmm_REGS];
static unsigned char pnc_map_signed[8];
static unsigned char pnc_map_unsigned[8];

static void build_reg_map(void)
{
	reg_gp_map[REG_GP_EAX] = 0x0;
	reg_gp_map[REG_GP_ECX] = 0x1;
	reg_gp_map[REG_GP_EDX] = 0x2;
	reg_gp_map[REG_GP_EBX] = 0x3;
	reg_gp_map[REG_GP_ESP] = 0x4;
	reg_gp_map[REG_GP_EBP] = 0x5;
	reg_gp_map[REG_GP_ESI] = 0x6;
	reg_gp_map[REG_GP_EDI] = 0x7;

	pnc_map_signed[pn_Cmp_Eq]    = 0x04;
	pnc_map_signed[pn_Cmp_Lt]    = 0x0C;
	pnc_map_signed[pn_Cmp_Le]    = 0x0E;
	pnc_map_signed[pn_Cmp_Gt]    = 0x0F;
	pnc_map_signed[pn_Cmp_Ge]    = 0x0D;
	pnc_map_signed[pn_Cmp_Lg]    = 0x05;

	pnc_map_unsigned[pn_Cmp_Eq]    = 0x04;
	pnc_map_unsigned[pn_Cmp_Lt]    = 0x02;
	pnc_map_unsigned[pn_Cmp_Le]    = 0x06;
	pnc_map_unsigned[pn_Cmp_Gt]    = 0x07;
	pnc_map_unsigned[pn_Cmp_Ge]    = 0x03;
	pnc_map_unsigned[pn_Cmp_Lg]    = 0x05;
}

/** Returns the encoding for a pnc field. */
static unsigned char pnc2cc(int pnc)
{
	unsigned char cc;
	if (pnc == ia32_pn_Cmp_parity) {
		cc = 0x0A;
	} else if (pnc & ia32_pn_Cmp_float || pnc & ia32_pn_Cmp_unsigned) {
		cc = pnc_map_unsigned[pnc & 0x07];
	} else {
		cc = pnc_map_signed[pnc & 0x07];
	}
	assert(cc != 0);
	return cc;
}

/** Sign extension bit values for binops */
enum SignExt {
	UNSIGNED_IMM = 0,  /**< unsigned immediate */
	SIGNEXT_IMM  = 2,  /**< sign extended immediate */
};

/** The mod encoding of the ModR/M */
enum Mod {
	MOD_IND          = 0x00, /**< [reg1] */
	MOD_IND_BYTE_OFS = 0x40, /**< [reg1 + byte ofs] */
	MOD_IND_WORD_OFS = 0x80, /**< [reg1 + word ofs] */
	MOD_REG          = 0xC0  /**< reg1 */
};

/** create R/M encoding for ModR/M */
#define ENC_RM(x) (x)
/** create REG encoding for ModR/M */
#define ENC_REG(x) ((x) << 3)

/** create encoding for a SIB byte */
#define ENC_SIB(scale, index, base) ((scale) << 6 | (index) << 3 | (base))

/* Node: The following routines are supposed to append bytes, words, dwords
   to the output stream.
   Currently the implementation is stupid in that it still creates output
   for an "assembler" in the form of .byte, .long
   We will change this when enough infrastructure is there to create complete
   machine code in memory/object files */

static void bemit8(const unsigned char byte)
{
	be_emit_irprintf("\t.byte 0x%x\n", byte);
	be_emit_write_line();
}

static void bemit16(const unsigned short u16)
{
	be_emit_irprintf("\t.word 0x%x\n", u16);
	be_emit_write_line();
}

static void bemit32(const unsigned u32)
{
	be_emit_irprintf("\t.long 0x%x\n", u32);
	be_emit_write_line();
}

/**
 * Emit address of an entity. If @p is_relative is true then a relative
 * offset from behind the address to the entity is created.
 */
static void bemit_entity(ir_entity *entity, bool entity_sign, int offset,
                         bool is_relative)
{
	if (entity == NULL) {
		bemit32(offset);
		return;
	}

	/* the final version should remember the position in the bytestream
	   and patch it with the correct address at linktime... */
	be_emit_cstring("\t.long ");
	if (entity_sign)
		be_emit_char('-');
	be_gas_emit_entity(entity);

	if (get_entity_owner(entity) == get_tls_type()) {
		if (get_entity_visibility(entity) == ir_visibility_external) {
			be_emit_cstring("@INDNTPOFF");
		} else {
			be_emit_cstring("@NTPOFF");
		}
	}

	if (is_relative) {
		be_emit_cstring("-.");
		offset -= 4;
	}

	if (offset != 0) {
		be_emit_irprintf("%+d", offset);
	}
	be_emit_char('\n');
	be_emit_write_line();
}

static void bemit_jmp_destination(const ir_node *dest_block)
{
	be_emit_cstring("\t.long ");
	be_gas_emit_block_name(dest_block);
	be_emit_cstring(" - . - 4\n");
	be_emit_write_line();
}

/* end emit routines, all emitters following here should only use the functions
   above. */

typedef enum reg_modifier {
	REG_LOW  = 0,
	REG_HIGH = 1
} reg_modifier_t;

/** Create a ModR/M byte for src1,src2 registers */
static void bemit_modrr(const arch_register_t *src1,
                        const arch_register_t *src2)
{
	unsigned char modrm = MOD_REG;
	modrm |= ENC_RM(reg_gp_map[src1->index]);
	modrm |= ENC_REG(reg_gp_map[src2->index]);
	bemit8(modrm);
}

/** Create a ModR/M8 byte for src1,src2 registers */
static void bemit_modrr8(reg_modifier_t high_part1, const arch_register_t *src1,
						 reg_modifier_t high_part2, const arch_register_t *src2)
{
	unsigned char modrm = MOD_REG;
	modrm |= ENC_RM(reg_gp_map[src1->index] +  (high_part1 == REG_HIGH ? 4 : 0));
	modrm |= ENC_REG(reg_gp_map[src2->index] + (high_part2 == REG_HIGH ? 4 : 0));
	bemit8(modrm);
}

/** Create a ModR/M byte for one register and extension */
static void bemit_modru(const arch_register_t *reg, unsigned ext)
{
	unsigned char modrm = MOD_REG;
	assert(ext <= 7);
	modrm |= ENC_RM(reg_gp_map[reg->index]);
	modrm |= ENC_REG(ext);
	bemit8(modrm);
}

/** Create a ModR/M8 byte for one register */
static void bemit_modrm8(reg_modifier_t high_part, const arch_register_t *reg)
{
	unsigned char modrm = MOD_REG;
	assert(reg_gp_map[reg->index] < 4);
	modrm |= ENC_RM(reg_gp_map[reg->index] + (high_part == REG_HIGH ? 4 : 0));
	modrm |= MOD_REG;
	bemit8(modrm);
}

/**
 * Calculate the size of an signed immediate in bytes.
 *
 * @param offset  an offset
 */
static unsigned get_signed_imm_size(int offset)
{
	if (-128 <= offset && offset < 128) {
		return 1;
	} else if (-32768 <= offset && offset < 32768) {
		return 2;
	} else {
		return 4;
	}
}

/**
 * Emit an address mode.
 *
 * @param reg   content of the reg field: either a register index or an opcode extension
 * @param node  the node
 */
static void bemit_mod_am(unsigned reg, const ir_node *node)
{
	ir_entity *ent       = get_ia32_am_sc(node);
	int        offs      = get_ia32_am_offs_int(node);
	ir_node   *base      = get_irn_n(node, n_ia32_base);
	int        has_base  = !is_ia32_NoReg_GP(base);
	ir_node   *index     = get_irn_n(node, n_ia32_index);
	int        has_index = !is_ia32_NoReg_GP(index);
	unsigned   modrm     = 0;
	unsigned   sib       = 0;
	unsigned   emitoffs  = 0;
	bool       emitsib   = false;
	unsigned   base_enc;

	/* set the mod part depending on displacement */
	if (ent != NULL) {
		modrm |= MOD_IND_WORD_OFS;
		emitoffs = 32;
	} else if (offs == 0) {
		modrm |= MOD_IND;
		emitoffs = 0;
	} else if (-128 <= offs && offs < 128) {
		modrm |= MOD_IND_BYTE_OFS;
		emitoffs = 8;
	} else {
		modrm |= MOD_IND_WORD_OFS;
		emitoffs = 32;
	}

	if (has_base) {
		const arch_register_t *base_reg = arch_get_irn_register(base);
		base_enc = reg_gp_map[base_reg->index];
	} else {
		/* Use the EBP encoding + MOD_IND if NO base register. There is
		 * always a 32bit offset present in this case. */
		modrm    = MOD_IND;
		base_enc = 0x05;
		emitoffs = 32;
	}

	/* Determine if we need a SIB byte. */
	if (has_index) {
		const arch_register_t *reg_index = arch_get_irn_register(index);
		int                    scale     = get_ia32_am_scale(node);
		assert(scale < 4);
		/* R/M set to ESP means SIB in 32bit mode. */
		modrm   |= ENC_RM(0x04);
		sib      = ENC_SIB(scale, reg_gp_map[reg_index->index], base_enc);
		emitsib = true;
	} else if (base_enc == 0x04) {
		/* for the above reason we are forced to emit a SIB when base is ESP.
		 * Only the base is used, index must be ESP too, which means no index.
		 */
		modrm   |= ENC_RM(0x04);
		sib      = ENC_SIB(0, 0x04, 0x04);
		emitsib  = true;
	} else {
		modrm |= ENC_RM(base_enc);
	}

	/* We are forced to emit an 8bit offset as EBP base without offset is a
	 * special case for SIB without base register. */
	if (base_enc == 0x05 && emitoffs == 0) {
		modrm    |= MOD_IND_BYTE_OFS;
		emitoffs  = 8;
	}

	modrm |= ENC_REG(reg);

	bemit8(modrm);
	if (emitsib)
		bemit8(sib);

	/* emit displacement */
	if (emitoffs == 8) {
		bemit8((unsigned) offs);
	} else if (emitoffs == 32) {
		bemit_entity(ent, is_ia32_am_sc_sign(node), offs, false);
	}
}

/**
 * Emit a binop with a immediate operand.
 *
 * @param node        the node to emit
 * @param opcode_eax  the opcode for the op eax, imm variant
 * @param opcode      the opcode for the reg, imm variant
 * @param ruval       the opcode extension for opcode
 */
static void bemit_binop_with_imm(
	const ir_node *node,
	unsigned char opcode_ax,
	unsigned char opcode, unsigned char ruval)
{
	/* Use in-reg, because some instructions (cmp, test) have no out-reg. */
	const ir_node               *op   = get_irn_n(node, n_ia32_binary_right);
	const ia32_immediate_attr_t *attr = get_ia32_immediate_attr_const(op);
	unsigned                     size;

	/* Some instructions (test) have no short form with 32bit value + 8bit
	 * immediate. */
	if (attr->symconst != NULL || opcode & SIGNEXT_IMM) {
		size = 4;
	} else {
		/* check for sign extension */
		size = get_signed_imm_size(attr->offset);
	}

	switch (size) {
	case 1:
		bemit8(opcode | SIGNEXT_IMM);
		/* cmp has this special mode */
		if (get_ia32_op_type(node) == ia32_AddrModeS) {
			bemit_mod_am(ruval, node);
		} else {
			const arch_register_t *reg = get_in_reg(node, n_ia32_binary_left);
			bemit_modru(reg, ruval);
		}
		bemit8((unsigned char)attr->offset);
		return;
	case 2:
	case 4:
		/* check for eax variant: this variant is shorter for 32bit immediates only */
		if (get_ia32_op_type(node) == ia32_AddrModeS) {
			bemit8(opcode);
			bemit_mod_am(ruval, node);
		} else {
			const arch_register_t *reg = get_in_reg(node, n_ia32_binary_left);
			if (reg->index == REG_GP_EAX) {
				bemit8(opcode_ax);
			} else {
				bemit8(opcode);
				bemit_modru(reg, ruval);
			}
		}
		bemit_entity(attr->symconst, attr->sc_sign, attr->offset, false);
		return;
	}
	panic("invalid imm size?!?");
}

/**
 * Emits a binop.
 */
static void bemit_binop_2(const ir_node *node, unsigned code)
{
	const arch_register_t *out = get_in_reg(node, n_ia32_binary_left);
	bemit8(code);
	if (get_ia32_op_type(node) == ia32_Normal) {
		const arch_register_t *op2 = get_in_reg(node, n_ia32_binary_right);
		bemit_modrr(op2, out);
	} else {
		bemit_mod_am(reg_gp_map[out->index], node);
	}
}

/**
 * Emit a binop.
 */
static void bemit_binop(const ir_node *node, const unsigned char opcodes[4])
{
	ir_node *right = get_irn_n(node, n_ia32_binary_right);
	if (is_ia32_Immediate(right)) {
		bemit_binop_with_imm(node, opcodes[1], opcodes[2], opcodes[3]);
	} else {
		bemit_binop_2(node, opcodes[0]);
	}
}

/**
 * Emit an unop.
 */
static void bemit_unop(const ir_node *node, unsigned char code, unsigned char ext, int input)
{
	bemit8(code);
	if (get_ia32_op_type(node) == ia32_Normal) {
		const arch_register_t *in = get_in_reg(node, input);
		bemit_modru(in, ext);
	} else {
		bemit_mod_am(ext, node);
	}
}

static void bemit_unop_reg(const ir_node *node, unsigned char code, int input)
{
	const arch_register_t *out = get_out_reg(node, 0);
	bemit_unop(node, code, reg_gp_map[out->index], input);
}

static void bemit_unop_mem(const ir_node *node, unsigned char code, unsigned char ext)
{
	unsigned size = get_mode_size_bits(get_ia32_ls_mode(node));
	if (size == 16)
		bemit8(0x66);
	bemit8(size == 8 ? code : code + 1);
	bemit_mod_am(ext, node);
}

static void bemit_immediate(const ir_node *node, bool relative)
{
	const ia32_immediate_attr_t *attr = get_ia32_immediate_attr_const(node);
	bemit_entity(attr->symconst, attr->sc_sign, attr->offset, relative);
}

static void bemit_copy(const ir_node *copy)
{
	const arch_register_t *in  = get_in_reg(copy, 0);
	const arch_register_t *out = get_out_reg(copy, 0);

	if (in == out)
		return;
	/* copies of vf nodes aren't real... */
	if (arch_register_get_class(in) == &ia32_reg_classes[CLASS_ia32_vfp])
		return;

	if (get_irn_mode(copy) == mode_E) {
		panic("NIY");
	} else {
		assert(arch_register_get_class(in) == &ia32_reg_classes[CLASS_ia32_gp]);
		bemit8(0x8B);
		bemit_modrr(in, out);
	}
}

static void bemit_perm(const ir_node *node)
{
	const arch_register_t       *in0  = arch_get_irn_register(get_irn_n(node, 0));
	const arch_register_t       *in1  = arch_get_irn_register(get_irn_n(node, 1));
	const arch_register_class_t *cls0 = arch_register_get_class(in0);

	assert(cls0 == arch_register_get_class(in1) && "Register class mismatch at Perm");

	if (cls0 == &ia32_reg_classes[CLASS_ia32_gp]) {
		if (in0->index == REG_GP_EAX) {
			bemit8(0x90 + reg_gp_map[in1->index]);
		} else if (in1->index == REG_GP_EAX) {
			bemit8(0x90 + reg_gp_map[in0->index]);
		} else {
			bemit8(0x87);
			bemit_modrr(in0, in1);
		}
	} else if (cls0 == &ia32_reg_classes[CLASS_ia32_xmm]) {
		panic("unimplemented"); // TODO implement
		//ia32_emitf(NULL, "\txorpd %R, %R\n", in1, in0);
		//ia32_emitf(NULL, "\txorpd %R, %R\n", in0, in1);
		//ia32_emitf(node, "\txorpd %R, %R\n", in1, in0);
	} else if (cls0 == &ia32_reg_classes[CLASS_ia32_vfp]) {
		/* is a NOP */
	} else if (cls0 == &ia32_reg_classes[CLASS_ia32_st]) {
		/* is a NOP */
	} else {
		panic("unexpected register class in be_Perm (%+F)", node);
	}
}

static void bemit_xor0(const ir_node *node)
{
	const arch_register_t *out = get_out_reg(node, 0);
	bemit8(0x31);
	bemit_modrr(out, out);
}

static void bemit_mov_const(const ir_node *node)
{
	const arch_register_t *out = get_out_reg(node, 0);
	bemit8(0xB8 + reg_gp_map[out->index]);
	bemit_immediate(node, false);
}

/**
 * Creates a function for a Binop with 3 possible encodings.
 */
#define BINOP(op, op0, op1, op2, op2_ext)                                 \
static void bemit_ ## op(const ir_node *node) {                           \
	static const unsigned char op ## _codes[] = {op0, op1, op2, op2_ext}; \
	bemit_binop(node, op ## _codes);                                      \
}

/*    insn  def  eax,imm   imm */
BINOP(add,  0x03, 0x05, 0x81, 0)
BINOP(or,   0x0B, 0x0D, 0x81, 1)
BINOP(adc,  0x13, 0x15, 0x81, 2)
BINOP(sbb,  0x1B, 0x1D, 0x81, 3)
BINOP(and,  0x23, 0x25, 0x81, 4)
BINOP(sub,  0x2B, 0x2D, 0x81, 5)
BINOP(xor,  0x33, 0x35, 0x81, 6)
BINOP(test, 0x85, 0xA9, 0xF7, 0)

#define BINOPMEM(op, ext) \
static void bemit_##op(const ir_node *node) \
{ \
	ir_node *val; \
	unsigned size = get_mode_size_bits(get_ia32_ls_mode(node)); \
	if (size == 16) \
		bemit8(0x66); \
	val = get_irn_n(node, n_ia32_unary_op); \
	if (is_ia32_Immediate(val)) { \
		const ia32_immediate_attr_t *attr   = get_ia32_immediate_attr_const(val); \
		int                          offset = attr->offset; \
		if (attr->symconst == NULL && get_signed_imm_size(offset) == 1) { \
			bemit8(0x83); \
			bemit_mod_am(ext, node); \
			bemit8(offset); \
		} else { \
			bemit8(0x81); \
			bemit_mod_am(ext, node); \
			if (size == 16) { \
				bemit16(offset); \
			} else { \
				bemit_entity(attr->symconst, attr->sc_sign, offset, false); \
			} \
		} \
	} else { \
		bemit8(ext << 3 | 1); \
		bemit_mod_am(reg_gp_map[get_out_reg(val, 0)->index], node); \
	} \
} \
 \
static void bemit_##op##8bit(const ir_node *node) \
{ \
	ir_node *val = get_irn_n(node, n_ia32_unary_op); \
	if (is_ia32_Immediate(val)) { \
		bemit8(0x80); \
		bemit_mod_am(ext, node); \
		bemit8(get_ia32_immediate_attr_const(val)->offset); \
	} else { \
		bemit8(ext << 3); \
		bemit_mod_am(reg_gp_map[get_out_reg(val, 0)->index], node); \
	} \
}

BINOPMEM(addmem,  0)
BINOPMEM(ormem,   1)
BINOPMEM(andmem,  4)
BINOPMEM(submem,  5)
BINOPMEM(xormem,  6)


/**
 * Creates a function for an Unop with code /ext encoding.
 */
#define UNOP(op, code, ext, input)              \
static void bemit_ ## op(const ir_node *node) { \
	bemit_unop(node, code, ext, input);         \
}

UNOP(not,     0xF7, 2, n_ia32_Not_val)
UNOP(neg,     0xF7, 3, n_ia32_Neg_val)
UNOP(mul,     0xF7, 4, n_ia32_Mul_right)
UNOP(imul1op, 0xF7, 5, n_ia32_IMul1OP_right)
UNOP(div,     0xF7, 6, n_ia32_Div_divisor)
UNOP(idiv,    0xF7, 7, n_ia32_IDiv_divisor)

/* TODO: am support for IJmp */
UNOP(ijmp,    0xFF, 4, n_ia32_IJmp_target)

#define SHIFT(op, ext) \
static void bemit_##op(const ir_node *node) \
{ \
	const arch_register_t *out   = get_out_reg(node, 0); \
	ir_node               *count = get_irn_n(node, 1); \
	if (is_ia32_Immediate(count)) { \
		int offset = get_ia32_immediate_attr_const(count)->offset; \
		if (offset == 1) { \
			bemit8(0xD1); \
			bemit_modru(out, ext); \
		} else { \
			bemit8(0xC1); \
			bemit_modru(out, ext); \
			bemit8(offset); \
		} \
	} else { \
		bemit8(0xD3); \
		bemit_modru(out, ext); \
	} \
} \
 \
static void bemit_##op##mem(const ir_node *node) \
{ \
	ir_node *count; \
	unsigned size = get_mode_size_bits(get_ia32_ls_mode(node)); \
	if (size == 16) \
		bemit8(0x66); \
	count = get_irn_n(node, 1); \
	if (is_ia32_Immediate(count)) { \
		int offset = get_ia32_immediate_attr_const(count)->offset; \
		if (offset == 1) { \
			bemit8(size == 8 ? 0xD0 : 0xD1); \
			bemit_mod_am(ext, node); \
		} else { \
			bemit8(size == 8 ? 0xC0 : 0xC1); \
			bemit_mod_am(ext, node); \
			bemit8(offset); \
		} \
	} else { \
		bemit8(size == 8 ? 0xD2 : 0xD3); \
		bemit_mod_am(ext, node); \
	} \
}

SHIFT(rol, 0)
SHIFT(ror, 1)
SHIFT(shl, 4)
SHIFT(shr, 5)
SHIFT(sar, 7)

static void bemit_shld(const ir_node *node)
{
	const arch_register_t *in  = get_in_reg(node, n_ia32_ShlD_val_low);
	const arch_register_t *out = get_out_reg(node, pn_ia32_ShlD_res);
	ir_node *count = get_irn_n(node, n_ia32_ShlD_count);
	bemit8(0x0F);
	if (is_ia32_Immediate(count)) {
		bemit8(0xA4);
		bemit_modrr(out, in);
		bemit8(get_ia32_immediate_attr_const(count)->offset);
	} else {
		bemit8(0xA5);
		bemit_modrr(out, in);
	}
}

static void bemit_shrd(const ir_node *node)
{
	const arch_register_t *in  = get_in_reg(node, n_ia32_ShrD_val_low);
	const arch_register_t *out = get_out_reg(node, pn_ia32_ShrD_res);
	ir_node *count = get_irn_n(node, n_ia32_ShrD_count);
	bemit8(0x0F);
	if (is_ia32_Immediate(count)) {
		bemit8(0xAC);
		bemit_modrr(out, in);
		bemit8(get_ia32_immediate_attr_const(count)->offset);
	} else {
		bemit8(0xAD);
		bemit_modrr(out, in);
	}
}

/**
 * binary emitter for setcc.
 */
static void bemit_setcc(const ir_node *node)
{
	const arch_register_t *dreg = get_out_reg(node, pn_ia32_Setcc_res);

	int pnc = get_ia32_condcode(node);
	pnc     = determine_final_pnc(node, n_ia32_Setcc_eflags, pnc);
	if (pnc & ia32_pn_Cmp_float) {
		switch (pnc & 0x0f) {
		case pn_Cmp_Uo:
			 /* setp <dreg */
			bemit8(0x0F);
			bemit8(0x9A);
			bemit_modrm8(REG_LOW, dreg);
			return;

		case pn_Cmp_Leg:
			 /* setnp <dreg*/
			bemit8(0x0F);
			bemit8(0x9B);
			bemit_modrm8(REG_LOW, dreg);
			return;

		case pn_Cmp_Eq:
		case pn_Cmp_Lt:
		case pn_Cmp_Le:
			 /* set%PNC <dreg */
			bemit8(0x0F);
			bemit8(0x90 | pnc2cc(pnc));
			bemit_modrm8(REG_LOW, dreg);

			/* setnp >dreg */
			bemit8(0x0F);
			bemit8(0x9B);
			bemit_modrm8(REG_HIGH, dreg);

			/* andb %>dreg, %<dreg */
			bemit8(0x20);
			bemit_modrr8(REG_LOW, dreg, REG_HIGH, dreg);
			return;

		case pn_Cmp_Ug:
		case pn_Cmp_Uge:
		case pn_Cmp_Ne:
			/* set%PNC <dreg */
			bemit8(0x0F);
			bemit8(0x90 | pnc2cc(pnc));
			bemit_modrm8(REG_LOW, dreg);

			/* setp >dreg */
			bemit8(0x0F);
			bemit8(0x9A);
			bemit_modrm8(REG_HIGH, dreg);

			/* orb %>dreg, %<dreg */
			bemit8(0x08);
			bemit_modrr8(REG_LOW, dreg, REG_HIGH, dreg);
			return;

		default:
			break;
		}
	}
	/* set%PNC <dreg */
	bemit8(0x0F);
	bemit8(0x90 | pnc2cc(pnc));
	bemit_modrm8(REG_LOW, dreg);
}

static void bemit_cmovcc(const ir_node *node)
{
	const ia32_attr_t     *attr         = get_ia32_attr_const(node);
	int                    ins_permuted = attr->data.ins_permuted;
	const arch_register_t *out          = arch_irn_get_register(node, pn_ia32_res);
	int                    pnc          = get_ia32_condcode(node);
	const arch_register_t *in_true;
	const arch_register_t *in_false;

	pnc = determine_final_pnc(node, n_ia32_CMovcc_eflags, pnc);

	in_true  = arch_get_irn_register(get_irn_n(node, n_ia32_CMovcc_val_true));
	in_false = arch_get_irn_register(get_irn_n(node, n_ia32_CMovcc_val_false));

	/* should be same constraint fullfilled? */
	if (out == in_false) {
		/* yes -> nothing to do */
	} else if (out == in_true) {
		assert(get_ia32_op_type(node) == ia32_Normal);
		ins_permuted = !ins_permuted;
		in_true      = in_false;
	} else {
		/* we need a mov */
		bemit8(0x8B); // mov %in_false, %out
		bemit_modrr(in_false, out);
	}

	if (ins_permuted)
		pnc = ia32_get_negated_pnc(pnc);

	/* TODO: handling of Nans isn't correct yet */

	bemit8(0x0F);
	bemit8(0x40 | pnc2cc(pnc));
	if (get_ia32_op_type(node) == ia32_Normal) {
		bemit_modrr(in_true, out);
	} else {
		bemit_mod_am(reg_gp_map[out->index], node);
	}
}

static void bemit_cmp(const ir_node *node)
{
	unsigned  ls_size = get_mode_size_bits(get_ia32_ls_mode(node));
	ir_node  *right;

	if (ls_size == 16)
		bemit8(0x66);

	right = get_irn_n(node, n_ia32_binary_right);
	if (is_ia32_Immediate(right)) {
		/* Use in-reg, because some instructions (cmp, test) have no out-reg. */
		const ir_node               *op   = get_irn_n(node, n_ia32_binary_right);
		const ia32_immediate_attr_t *attr = get_ia32_immediate_attr_const(op);
		unsigned                     size;

		if (attr->symconst != NULL) {
			size = 4;
		} else {
			/* check for sign extension */
			size = get_signed_imm_size(attr->offset);
		}

		switch (size) {
			case 1:
				bemit8(0x81 | SIGNEXT_IMM);
				/* cmp has this special mode */
				if (get_ia32_op_type(node) == ia32_AddrModeS) {
					bemit_mod_am(7, node);
				} else {
					const arch_register_t *reg = get_in_reg(node, n_ia32_binary_left);
					bemit_modru(reg, 7);
				}
				bemit8((unsigned char)attr->offset);
				return;
			case 2:
			case 4:
				/* check for eax variant: this variant is shorter for 32bit immediates only */
				if (get_ia32_op_type(node) == ia32_AddrModeS) {
					bemit8(0x81);
					bemit_mod_am(7, node);
				} else {
					const arch_register_t *reg = get_in_reg(node, n_ia32_binary_left);
					if (reg->index == REG_GP_EAX) {
						bemit8(0x3D);
					} else {
						bemit8(0x81);
						bemit_modru(reg, 7);
					}
				}
				if (ls_size == 16) {
					bemit16(attr->offset);
				} else {
					bemit_entity(attr->symconst, attr->sc_sign, attr->offset, false);
				}
				return;
		}
		panic("invalid imm size?!?");
	} else {
		const arch_register_t *out = get_in_reg(node, n_ia32_binary_left);
		bemit8(0x3B);
		if (get_ia32_op_type(node) == ia32_Normal) {
			const arch_register_t *op2 = get_in_reg(node, n_ia32_binary_right);
			bemit_modrr(op2, out);
		} else {
			bemit_mod_am(reg_gp_map[out->index], node);
		}
	}
}

static void bemit_cmp8bit(const ir_node *node)
{
	ir_node *right = get_irn_n(node, n_ia32_binary_right);
	if (is_ia32_Immediate(right)) {
		if (get_ia32_op_type(node) == ia32_Normal) {
			const arch_register_t *out = get_in_reg(node, n_ia32_Cmp_left);
			if (out->index == REG_GP_EAX) {
				bemit8(0x3C);
			} else {
				bemit8(0x80);
				bemit_modru(out, 7);
			}
		} else {
			bemit8(0x80);
			bemit_mod_am(7, node);
		}
		bemit8(get_ia32_immediate_attr_const(right)->offset);
	} else {
		const arch_register_t *out = get_in_reg(node, n_ia32_Cmp_left);
		bemit8(0x3A);
		if (get_ia32_op_type(node) == ia32_Normal) {
			const arch_register_t *in = get_in_reg(node, n_ia32_Cmp_right);
			bemit_modrr(out, in);
		} else {
			bemit_mod_am(reg_gp_map[out->index], node);
		}
	}
}

static void bemit_test8bit(const ir_node *node)
{
	ir_node *right = get_irn_n(node, n_ia32_Test8Bit_right);
	if (is_ia32_Immediate(right)) {
		if (get_ia32_op_type(node) == ia32_Normal) {
			const arch_register_t *out = get_in_reg(node, n_ia32_Test8Bit_left);
			if (out->index == REG_GP_EAX) {
				bemit8(0xA8);
			} else {
				bemit8(0xF6);
				bemit_modru(out, 0);
			}
		} else {
			bemit8(0xF6);
			bemit_mod_am(0, node);
		}
		bemit8(get_ia32_immediate_attr_const(right)->offset);
	} else {
		const arch_register_t *out = get_in_reg(node, n_ia32_Test8Bit_left);
		bemit8(0x84);
		if (get_ia32_op_type(node) == ia32_Normal) {
			const arch_register_t *in = get_in_reg(node, n_ia32_Test8Bit_right);
			bemit_modrr(out, in);
		} else {
			bemit_mod_am(reg_gp_map[out->index], node);
		}
	}
}

static void bemit_imul(const ir_node *node)
{
	ir_node *right = get_irn_n(node, n_ia32_IMul_right);
	/* Do we need the immediate form? */
	if (is_ia32_Immediate(right)) {
		int imm = get_ia32_immediate_attr_const(right)->offset;
		if (get_signed_imm_size(imm) == 1) {
			bemit_unop_reg(node, 0x6B, n_ia32_IMul_left);
			bemit8(imm);
		} else {
			bemit_unop_reg(node, 0x69, n_ia32_IMul_left);
			bemit32(imm);
		}
	} else {
		bemit8(0x0F);
		bemit_unop_reg(node, 0xAF, n_ia32_IMul_right);
	}
}

static void bemit_dec(const ir_node *node)
{
	const arch_register_t *out = get_out_reg(node, pn_ia32_Dec_res);
	bemit8(0x48 + reg_gp_map[out->index]);
}

static void bemit_inc(const ir_node *node)
{
	const arch_register_t *out = get_out_reg(node, pn_ia32_Inc_res);
	bemit8(0x40 + reg_gp_map[out->index]);
}

#define UNOPMEM(op, code, ext) \
static void bemit_##op(const ir_node *node) \
{ \
	bemit_unop_mem(node, code, ext); \
}

UNOPMEM(notmem, 0xF6, 2)
UNOPMEM(negmem, 0xF6, 3)
UNOPMEM(incmem, 0xFE, 0)
UNOPMEM(decmem, 0xFE, 1)

static void bemit_ldtls(const ir_node *node)
{
	const arch_register_t *out = get_out_reg(node, 0);

	bemit8(0x65); // gs:
	if (out->index == REG_GP_EAX) {
		bemit8(0xA1); // movl 0, %eax
	} else {
		bemit8(0x8B); // movl 0, %reg
		bemit8(MOD_IND | ENC_REG(reg_gp_map[out->index]) | ENC_RM(0x05));
	}
	bemit32(0);
}

/**
 * Emit a Lea.
 */
static void bemit_lea(const ir_node *node)
{
	const arch_register_t *out = get_out_reg(node, 0);
	bemit8(0x8D);
	bemit_mod_am(reg_gp_map[out->index], node);
}

/* helper function for bemit_minus64bit */
static void bemit_helper_mov(const arch_register_t *src, const arch_register_t *dst)
{
	bemit8(0x8B); // movl %src, %dst
	bemit_modrr(src, dst);
}

/* helper function for bemit_minus64bit */
static void bemit_helper_neg(const arch_register_t *reg)
{
	bemit8(0xF7); // negl %reg
	bemit_modru(reg, 3);
}

/* helper function for bemit_minus64bit */
static void bemit_helper_sbb0(const arch_register_t *reg)
{
	bemit8(0x83); // sbbl $0, %reg
	bemit_modru(reg, 3);
	bemit8(0);
}

/* helper function for bemit_minus64bit */
static void bemit_helper_sbb(const arch_register_t *src, const arch_register_t *dst)
{
	bemit8(0x1B); // sbbl %src, %dst
	bemit_modrr(src, dst);
}

/* helper function for bemit_minus64bit */
static void bemit_helper_xchg(const arch_register_t *src, const arch_register_t *dst)
{
	if (src->index == REG_GP_EAX) {
		bemit8(0x90 + reg_gp_map[dst->index]); // xchgl %eax, %dst
	} else if (dst->index == REG_GP_EAX) {
		bemit8(0x90 + reg_gp_map[src->index]); // xchgl %src, %eax
	} else {
		bemit8(0x87); // xchgl %src, %dst
		bemit_modrr(src, dst);
	}
}

/* helper function for bemit_minus64bit */
static void bemit_helper_zero(const arch_register_t *reg)
{
	bemit8(0x33); // xorl %reg, %reg
	bemit_modrr(reg, reg);
}

static void bemit_minus64bit(const ir_node *node)
{
	const arch_register_t *in_lo  = get_in_reg(node, 0);
	const arch_register_t *in_hi  = get_in_reg(node, 1);
	const arch_register_t *out_lo = get_out_reg(node, 0);
	const arch_register_t *out_hi = get_out_reg(node, 1);

	if (out_lo == in_lo) {
		if (out_hi != in_hi) {
			/* a -> a, b -> d */
			goto zero_neg;
		} else {
			/* a -> a, b -> b */
			goto normal_neg;
		}
	} else if (out_lo == in_hi) {
		if (out_hi == in_lo) {
			/* a -> b, b -> a */
			bemit_helper_xchg(in_lo, in_hi);
			goto normal_neg;
		} else {
			/* a -> b, b -> d */
			bemit_helper_mov(in_hi, out_hi);
			bemit_helper_mov(in_lo, out_lo);
			goto normal_neg;
		}
	} else {
		if (out_hi == in_lo) {
			/* a -> c, b -> a */
			bemit_helper_mov(in_lo, out_lo);
			goto zero_neg;
		} else if (out_hi == in_hi) {
			/* a -> c, b -> b */
			bemit_helper_mov(in_lo, out_lo);
			goto normal_neg;
		} else {
			/* a -> c, b -> d */
			bemit_helper_mov(in_lo, out_lo);
			goto zero_neg;
		}
	}

normal_neg:
	bemit_helper_neg( out_hi);
	bemit_helper_neg( out_lo);
	bemit_helper_sbb0(out_hi);
	return;

zero_neg:
	bemit_helper_zero(out_hi);
	bemit_helper_neg( out_lo);
	bemit_helper_sbb( in_hi, out_hi);
}

/**
 * Emit a single opcode.
 */
#define EMIT_SINGLEOP(op, code)                 \
static void bemit_ ## op(const ir_node *node) { \
	(void) node;                                \
	bemit8(code);                               \
}

//EMIT_SINGLEOP(daa,  0x27)
//EMIT_SINGLEOP(das,  0x2F)
//EMIT_SINGLEOP(aaa,  0x37)
//EMIT_SINGLEOP(aas,  0x3F)
//EMIT_SINGLEOP(nop,  0x90)
EMIT_SINGLEOP(cwtl,  0x98)
EMIT_SINGLEOP(cltd,  0x99)
//EMIT_SINGLEOP(fwait, 0x9B)
EMIT_SINGLEOP(sahf,  0x9E)
//EMIT_SINGLEOP(popf, 0x9D)
EMIT_SINGLEOP(leave, 0xC9)
EMIT_SINGLEOP(int3,  0xCC)
//EMIT_SINGLEOP(iret, 0xCF)
//EMIT_SINGLEOP(xlat, 0xD7)
//EMIT_SINGLEOP(lock, 0xF0)
EMIT_SINGLEOP(rep,   0xF3)
//EMIT_SINGLEOP(halt, 0xF4)
EMIT_SINGLEOP(cmc,   0xF5)
EMIT_SINGLEOP(stc,   0xF9)
//EMIT_SINGLEOP(cli,  0xFA)
//EMIT_SINGLEOP(sti,  0xFB)
//EMIT_SINGLEOP(std,  0xFD)

/**
 * Emits a MOV out, [MEM].
 */
static void bemit_load(const ir_node *node)
{
	const arch_register_t *out = get_out_reg(node, 0);

	if (out->index == REG_GP_EAX) {
		ir_node   *base      = get_irn_n(node, n_ia32_base);
		int        has_base  = !is_ia32_NoReg_GP(base);
		ir_node   *index     = get_irn_n(node, n_ia32_index);
		int        has_index = !is_ia32_NoReg_GP(index);
		if (!has_base && !has_index) {
			ir_entity *ent  = get_ia32_am_sc(node);
			int        offs = get_ia32_am_offs_int(node);
			/* load from constant address to EAX can be encoded
			   as 0xA1 [offset] */
			bemit8(0xA1);
			bemit_entity(ent, 0, offs, false);
			return;
		}
	}
	bemit8(0x8B);
	bemit_mod_am(reg_gp_map[out->index], node);
}

/**
 * Emits a MOV [mem], in.
 */
static void bemit_store(const ir_node *node)
{
	const ir_node *value = get_irn_n(node, n_ia32_Store_val);
	unsigned       size  = get_mode_size_bits(get_ia32_ls_mode(node));

	if (is_ia32_Immediate(value)) {
		if (size == 8) {
			bemit8(0xC6);
			bemit_mod_am(0, node);
			bemit8(get_ia32_immediate_attr_const(value)->offset);
		} else if (size == 16) {
			bemit8(0x66);
			bemit8(0xC7);
			bemit_mod_am(0, node);
			bemit16(get_ia32_immediate_attr_const(value)->offset);
		} else {
			bemit8(0xC7);
			bemit_mod_am(0, node);
			bemit_immediate(value, false);
		}
	} else {
		const arch_register_t *in = get_in_reg(node, n_ia32_Store_val);

		if (in->index == REG_GP_EAX) {
			ir_node   *base      = get_irn_n(node, n_ia32_base);
			int        has_base  = !is_ia32_NoReg_GP(base);
			ir_node   *index     = get_irn_n(node, n_ia32_index);
			int        has_index = !is_ia32_NoReg_GP(index);
			if (!has_base && !has_index) {
				ir_entity *ent  = get_ia32_am_sc(node);
				int        offs = get_ia32_am_offs_int(node);
				/* store to constant address from EAX can be encoded as
				 * 0xA2/0xA3 [offset]*/
				if (size == 8) {
					bemit8(0xA2);
				} else {
					if (size == 16)
						bemit8(0x66);
					bemit8(0xA3);
				}
				bemit_entity(ent, 0, offs, false);
				return;
			}
		}

		if (size == 8) {
			bemit8(0x88);
		} else {
			if (size == 16)
				bemit8(0x66);
			bemit8(0x89);
		}
		bemit_mod_am(reg_gp_map[in->index], node);
	}
}

static void bemit_conv_i2i(const ir_node *node)
{
	ir_mode  *smaller_mode = get_ia32_ls_mode(node);
	unsigned  opcode;

	bemit8(0x0F);
	/*        8 16 bit source
	 * movzx B6 B7
	 * movsx BE BF
	 */
	opcode = 0xB6;
	if (mode_is_signed(smaller_mode))           opcode |= 0x08;
	if (get_mode_size_bits(smaller_mode) == 16) opcode |= 0x01;
	bemit_unop_reg(node, opcode, n_ia32_Conv_I2I_val);
}

/**
 * Emit a Push.
 */
static void bemit_push(const ir_node *node)
{
	const ir_node *value = get_irn_n(node, n_ia32_Push_val);

	if (is_ia32_Immediate(value)) {
		const ia32_immediate_attr_t *attr
			= get_ia32_immediate_attr_const(value);
		unsigned size = get_signed_imm_size(attr->offset);
		if (attr->symconst)
			size = 4;
		switch (size) {
		case 1:
			bemit8(0x6A);
			bemit8((unsigned char)attr->offset);
			break;
		case 2:
		case 4:
			bemit8(0x68);
			bemit_immediate(value, false);
			break;
		}
	} else if (is_ia32_NoReg_GP(value)) {
		bemit8(0xFF);
		bemit_mod_am(6, node);
	} else {
		const arch_register_t *reg = get_in_reg(node, n_ia32_Push_val);
		bemit8(0x50 + reg_gp_map[reg->index]);
	}
}

/**
 * Emit a Pop.
 */
static void bemit_pop(const ir_node *node)
{
	const arch_register_t *reg = get_out_reg(node, pn_ia32_Pop_res);
	bemit8(0x58 + reg_gp_map[reg->index]);
}

static void bemit_popmem(const ir_node *node)
{
	bemit8(0x8F);
	bemit_mod_am(0, node);
}

static void bemit_call(const ir_node *node)
{
	ir_node *proc = get_irn_n(node, n_ia32_Call_addr);

	if (is_ia32_Immediate(proc)) {
		bemit8(0xE8);
		bemit_immediate(proc, true);
	} else {
		bemit_unop(node, 0xFF, 2, n_ia32_Call_addr);
	}
}

static void bemit_jmp(const ir_node *dest_block)
{
	bemit8(0xE9);
	bemit_jmp_destination(dest_block);
}

static void bemit_jump(const ir_node *node)
{
	if (can_be_fallthrough(node))
		return;

	bemit_jmp(get_cfop_target_block(node));
}

static void bemit_jcc(int pnc, const ir_node *dest_block)
{
	unsigned char cc = pnc2cc(pnc);
	bemit8(0x0F);
	bemit8(0x80 + cc);
	bemit_jmp_destination(dest_block);
}

static void bemit_jp(bool odd, const ir_node *dest_block)
{
	bemit8(0x0F);
	bemit8(0x8A + odd);
	bemit_jmp_destination(dest_block);
}

static void bemit_ia32_jcc(const ir_node *node)
{
	int            pnc = get_ia32_condcode(node);
	const ir_node *proj_true;
	const ir_node *proj_false;
	const ir_node *dest_true;
	const ir_node *dest_false;
	const ir_node *block;

	pnc = determine_final_pnc(node, 0, pnc);

	/* get both Projs */
	proj_true = get_proj(node, pn_ia32_Jcc_true);
	assert(proj_true && "Jcc without true Proj");

	proj_false = get_proj(node, pn_ia32_Jcc_false);
	assert(proj_false && "Jcc without false Proj");

	block = get_nodes_block(node);

	if (can_be_fallthrough(proj_true)) {
		/* exchange both proj's so the second one can be omitted */
		const ir_node *t = proj_true;

		proj_true  = proj_false;
		proj_false = t;
		pnc        = ia32_get_negated_pnc(pnc);
	}

	dest_true  = get_cfop_target_block(proj_true);
	dest_false = get_cfop_target_block(proj_false);

	if (pnc & ia32_pn_Cmp_float) {
		/* Some floating point comparisons require a test of the parity flag,
		 * which indicates that the result is unordered */
		switch (pnc & 15) {
			case pn_Cmp_Uo: {
				bemit_jp(false, dest_true);
				break;
			}

			case pn_Cmp_Leg:
				bemit_jp(true, dest_true);
				break;

			case pn_Cmp_Eq:
			case pn_Cmp_Lt:
			case pn_Cmp_Le:
				/* we need a local label if the false proj is a fallthrough
				 * as the falseblock might have no label emitted then */
				if (can_be_fallthrough(proj_false)) {
					bemit8(0x7A);
					bemit8(0x06);  // jp + 6
				} else {
					bemit_jp(false, dest_false);
				}
				goto emit_jcc;

			case pn_Cmp_Ug:
			case pn_Cmp_Uge:
			case pn_Cmp_Ne:
				bemit_jp(false, dest_true);
				goto emit_jcc;

			default:
				goto emit_jcc;
		}
	} else {
emit_jcc:
		bemit_jcc(pnc, dest_true);
	}

	/* the second Proj might be a fallthrough */
	if (can_be_fallthrough(proj_false)) {
		/* it's a fallthrough */
	} else {
		bemit_jmp(dest_false);
	}
}

static void bemit_switchjmp(const ir_node *node)
{
	unsigned long          interval;
	int                    last_value;
	int                    i;
	jmp_tbl_t              tbl;
	const arch_register_t *in;

	/* fill the table structure */
	generate_jump_table(&tbl, node);

	/* two-complement's magic make this work without overflow */
	interval = tbl.max_value - tbl.min_value;

	in = get_in_reg(node, 0);
	/* emit the table */
	if (get_signed_imm_size(interval) == 1) {
		bemit8(0x83); // cmpl $imm8, %in
		bemit_modru(in, 7);
		bemit8(interval);
	} else {
		bemit8(0x81); // cmpl $imm32, %in
		bemit_modru(in, 7);
		bemit32(interval);
	}
	bemit8(0x0F); // ja tbl.defProj
	bemit8(0x87);
	ia32_emitf(tbl.defProj, ".long %L - . - 4\n");

	if (tbl.num_branches > 1) {
		/* create table */
		bemit8(0xFF); // jmp *tbl.label(,%in,4)
		bemit8(MOD_IND | ENC_REG(4) | ENC_RM(0x04));
		bemit8(ENC_SIB(2, reg_gp_map[in->index], 0x05));
		be_emit_irprintf("\t.long %s\n", tbl.label);

		be_gas_emit_switch_section(GAS_SECTION_RODATA);
		be_emit_cstring(".align 4\n");
		be_emit_irprintf("%s:\n", tbl.label);

		last_value = tbl.branches[0].value;
		for (i = 0; i != tbl.num_branches; ++i) {
			while (last_value != tbl.branches[i].value) {
				ia32_emitf(tbl.defProj, ".long %L\n");
				++last_value;
			}
			ia32_emitf(tbl.branches[i].target, ".long %L\n");
			++last_value;
		}
		be_gas_emit_switch_section(GAS_SECTION_TEXT);
	} else {
		/* one jump is enough */
		panic("switch only has one case");
		//ia32_emitf(tbl.branches[0].target, "\tjmp %L\n");
	}

	be_emit_write_line();

	free(tbl.branches);
}

/**
 * Emits a return.
 */
static void bemit_return(const ir_node *node)
{
	unsigned pop = be_Return_get_pop(node);
	if (pop > 0 || be_Return_get_emit_pop(node)) {
		bemit8(0xC2);
		assert(pop <= 0xffff);
		bemit16(pop);
	} else {
		bemit8(0xC3);
	}
}

static void bemit_subsp(const ir_node *node)
{
	const arch_register_t *out;
	/* sub %in, %esp */
	bemit_sub(node);
	/* mov %esp, %out */
	bemit8(0x8B);
	out = get_out_reg(node, 1);
	bemit8(MOD_REG | ENC_REG(reg_gp_map[out->index]) | ENC_RM(0x04));
}

static void bemit_incsp(const ir_node *node)
{
	int                    offs;
	const arch_register_t *reg;
	unsigned               size;
	unsigned               ext;

	offs = be_get_IncSP_offset(node);
	if (offs == 0)
		return;

	if (offs > 0) {
		ext = 5; /* sub */
	} else {
		ext = 0; /* add */
		offs = -offs;
	}

	size = get_signed_imm_size(offs);
	bemit8(size == 1 ? 0x83 : 0x81);

	reg  = get_out_reg(node, 0);
	bemit_modru(reg, ext);

	if (size == 1) {
		bemit8(offs);
	} else {
		bemit32(offs);
	}
}

static void bemit_copybi(const ir_node *node)
{
	unsigned size = get_ia32_copyb_size(node);
	if (size & 1)
		bemit8(0xA4); // movsb
	if (size & 2) {
		bemit8(0x66);
		bemit8(0xA5); // movsw
	}
	size >>= 2;
	while (size--) {
		bemit8(0xA5); // movsl
	}
}

static void bemit_fbinop(const ir_node *node, unsigned code, unsigned code_to)
{
	if (get_ia32_op_type(node) == ia32_Normal) {
		const ia32_x87_attr_t *x87_attr = get_ia32_x87_attr_const(node);
		const arch_register_t *in1      = x87_attr->x87[0];
		const arch_register_t *in       = x87_attr->x87[1];
		const arch_register_t *out      = x87_attr->x87[2];

		if (out == NULL) {
			out = in1;
		} else if (out == in) {
			in = in1;
		}

		if (out->index == 0) {
			bemit8(0xD8);
			bemit8(MOD_REG | ENC_REG(code) | ENC_RM(in->index));
		} else {
			bemit8(0xDC);
			bemit8(MOD_REG | ENC_REG(code_to) | ENC_RM(out->index));
		}
	} else {
		if (get_mode_size_bits(get_ia32_ls_mode(node)) == 32) {
			bemit8(0xD8);
		} else {
			bemit8(0xDC);
		}
		bemit_mod_am(code, node);
	}
}

static void bemit_fbinopp(const ir_node *node, unsigned const code)
{
	const ia32_x87_attr_t *x87_attr = get_ia32_x87_attr_const(node);
	const arch_register_t *out      = x87_attr->x87[2];
	bemit8(0xDE);
	bemit8(code + out->index);
}

static void bemit_fabs(const ir_node *node)
{
	(void)node;

	bemit8(0xD9);
	bemit8(0xE1);
}

static void bemit_fadd(const ir_node *node)
{
	bemit_fbinop(node, 0, 0);
}

static void bemit_faddp(const ir_node *node)
{
	bemit_fbinopp(node, 0xC0);
}

static void bemit_fchs(const ir_node *node)
{
	(void)node;

	bemit8(0xD9);
	bemit8(0xE0);
}

static void bemit_fdiv(const ir_node *node)
{
	bemit_fbinop(node, 6, 7);
}

static void bemit_fdivp(const ir_node *node)
{
	bemit_fbinopp(node, 0xF8);
}

static void bemit_fdivr(const ir_node *node)
{
	bemit_fbinop(node, 7, 6);
}

static void bemit_fdivrp(const ir_node *node)
{
	bemit_fbinopp(node, 0xF0);
}

static void bemit_fild(const ir_node *node)
{
	switch (get_mode_size_bits(get_ia32_ls_mode(node))) {
		case 16:
			bemit8(0xDF); // filds
			bemit_mod_am(0, node);
			return;

		case 32:
			bemit8(0xDB); // fildl
			bemit_mod_am(0, node);
			return;

		case 64:
			bemit8(0xDF); // fildll
			bemit_mod_am(5, node);
			return;

		default:
			panic("invalid mode size");
	}
}

static void bemit_fist(const ir_node *node)
{
	switch (get_mode_size_bits(get_ia32_ls_mode(node))) {
		case 16:
			bemit8(0xDF); // fists
			break;

		case 32:
			bemit8(0xDB); // fistl
			break;

		default:
			panic("invalid mode size");
	}
	bemit_mod_am(2, node);
}

static void bemit_fistp(const ir_node *node)
{
	switch (get_mode_size_bits(get_ia32_ls_mode(node))) {
		case 16:
			bemit8(0xDF); // fistps
			bemit_mod_am(3, node);
			return;

		case 32:
			bemit8(0xDB); // fistpl
			bemit_mod_am(3, node);
			return;

		case 64:
			bemit8(0xDF); // fistpll
			bemit_mod_am(7, node);
			return;

		default:
			panic("invalid mode size");
	}
}

static void bemit_fld(const ir_node *node)
{
	switch (get_mode_size_bits(get_ia32_ls_mode(node))) {
		case 32:
			bemit8(0xD9); // flds
			bemit_mod_am(0, node);
			return;

		case 64:
			bemit8(0xDD); // fldl
			bemit_mod_am(0, node);
			return;

		case 80:
		case 96:
			bemit8(0xDB); // fldt
			bemit_mod_am(5, node);
			return;

		default:
			panic("invalid mode size");
	}
}

static void bemit_fld1(const ir_node *node)
{
	(void)node;
	bemit8(0xD9);
	bemit8(0xE8); // fld1
}

static void bemit_fldcw(const ir_node *node)
{
	bemit8(0xD9); // fldcw
	bemit_mod_am(5, node);
}

static void bemit_fldz(const ir_node *node)
{
	(void)node;
	bemit8(0xD9);
	bemit8(0xEE); // fldz
}

static void bemit_fmul(const ir_node *node)
{
	bemit_fbinop(node, 1, 1);
}

static void bemit_fmulp(const ir_node *node)
{
	bemit_fbinopp(node, 0xC8);
}

static void bemit_fpop(const ir_node *node)
{
	const ia32_x87_attr_t *attr = get_ia32_x87_attr_const(node);
	bemit8(0xDD);
	bemit8(0xD8 + attr->x87[0]->index);
}

static void bemit_fpush(const ir_node *node)
{
	const ia32_x87_attr_t *attr = get_ia32_x87_attr_const(node);
	bemit8(0xD9);
	bemit8(0xC0 + attr->x87[0]->index);
}

static void bemit_fpushcopy(const ir_node *node)
{
	const ia32_x87_attr_t *attr = get_ia32_x87_attr_const(node);
	bemit8(0xD9);
	bemit8(0xC0 + attr->x87[0]->index);
}

static void bemit_fst(const ir_node *node)
{
	switch (get_mode_size_bits(get_ia32_ls_mode(node))) {
		case 32:
			bemit8(0xD9); // fsts
			break;

		case 64:
			bemit8(0xDD); // fstl
			break;

		default:
			panic("invalid mode size");
	}
	bemit_mod_am(2, node);
}

static void bemit_fstp(const ir_node *node)
{
	switch (get_mode_size_bits(get_ia32_ls_mode(node))) {
		case 32:
			bemit8(0xD9); // fstps
			bemit_mod_am(3, node);
			return;

		case 64:
			bemit8(0xDD); // fstpl
			bemit_mod_am(3, node);
			return;

		case 80:
		case 96:
			bemit8(0xDB); // fstpt
			bemit_mod_am(7, node);
			return;

		default:
			panic("invalid mode size");
	}
}

static void bemit_fsub(const ir_node *node)
{
	bemit_fbinop(node, 4, 5);
}

static void bemit_fsubp(const ir_node *node)
{
	bemit_fbinopp(node, 0xE8);
}

static void bemit_fsubr(const ir_node *node)
{
	bemit_fbinop(node, 5, 4);
}

static void bemit_fsubrp(const ir_node *node)
{
	bemit_fbinopp(node, 0xE0);
}

static void bemit_fnstcw(const ir_node *node)
{
	bemit8(0xD9); // fnstcw
	bemit_mod_am(7, node);
}

static void bemit_fnstsw(void)
{
	bemit8(0xDF); // fnstsw %ax
	bemit8(0xE0);
}

static void bemit_ftstfnstsw(const ir_node *node)
{
	(void)node;

	bemit8(0xD9); // ftst
	bemit8(0xE4);
	bemit_fnstsw();
}

static void bemit_fucomi(const ir_node *node)
{
	const ia32_x87_attr_t *attr = get_ia32_x87_attr_const(node);
	bemit8(0xDB); // fucomi
	bemit8(0xE8 + attr->x87[1]->index);
}

static void bemit_fucomip(const ir_node *node)
{
	const ia32_x87_attr_t *attr = get_ia32_x87_attr_const(node);
	bemit8(0xDF); // fucomip
	bemit8(0xE8 + attr->x87[1]->index);
}

static void bemit_fucomfnstsw(const ir_node *node)
{
	const ia32_x87_attr_t *attr = get_ia32_x87_attr_const(node);
	bemit8(0xDD); // fucom
	bemit8(0xE0 + attr->x87[1]->index);
	bemit_fnstsw();
}

static void bemit_fucompfnstsw(const ir_node *node)
{
	const ia32_x87_attr_t *attr = get_ia32_x87_attr_const(node);
	bemit8(0xDD); // fucomp
	bemit8(0xE8 + attr->x87[1]->index);
	bemit_fnstsw();
}

static void bemit_fucomppfnstsw(const ir_node *node)
{
	(void)node;

	bemit8(0xDA); // fucompp
	bemit8(0xE9);
	bemit_fnstsw();
}

static void bemit_fxch(const ir_node *node)
{
	const ia32_x87_attr_t *attr = get_ia32_x87_attr_const(node);
	bemit8(0xD9);
	bemit8(0xC8 + attr->x87[0]->index);
}

/**
 * The type of a emitter function.
 */
typedef void (*emit_func) (const ir_node *);

/**
 * Set a node emitter. Make it a bit more type safe.
 */
static void register_emitter(ir_op *op, emit_func func)
{
	op->ops.generic = (op_func) func;
}

static void ia32_register_binary_emitters(void)
{
	/* first clear the generic function pointer for all ops */
	clear_irp_opcodes_generic_func();

	/* benode emitter */
	register_emitter(op_be_Copy,            bemit_copy);
	register_emitter(op_be_CopyKeep,        bemit_copy);
	register_emitter(op_be_IncSP,           bemit_incsp);
	register_emitter(op_be_Perm,            bemit_perm);
	register_emitter(op_be_Return,          bemit_return);
	register_emitter(op_ia32_Adc,           bemit_adc);
	register_emitter(op_ia32_Add,           bemit_add);
	register_emitter(op_ia32_AddMem,        bemit_addmem);
	register_emitter(op_ia32_AddMem8Bit,    bemit_addmem8bit);
	register_emitter(op_ia32_And,           bemit_and);
	register_emitter(op_ia32_AndMem,        bemit_andmem);
	register_emitter(op_ia32_AndMem8Bit,    bemit_andmem8bit);
	register_emitter(op_ia32_Breakpoint,    bemit_int3);
	register_emitter(op_ia32_CMovcc,        bemit_cmovcc);
	register_emitter(op_ia32_Call,          bemit_call);
	register_emitter(op_ia32_Cltd,          bemit_cltd);
	register_emitter(op_ia32_Cmc,           bemit_cmc);
	register_emitter(op_ia32_Cmp,           bemit_cmp);
	register_emitter(op_ia32_Cmp8Bit,       bemit_cmp8bit);
	register_emitter(op_ia32_Const,         bemit_mov_const);
	register_emitter(op_ia32_Conv_I2I,      bemit_conv_i2i);
	register_emitter(op_ia32_Conv_I2I8Bit,  bemit_conv_i2i);
	register_emitter(op_ia32_CopyB_i,       bemit_copybi);
	register_emitter(op_ia32_Cwtl,          bemit_cwtl);
	register_emitter(op_ia32_Dec,           bemit_dec);
	register_emitter(op_ia32_DecMem,        bemit_decmem);
	register_emitter(op_ia32_Div,           bemit_div);
	register_emitter(op_ia32_FldCW,         bemit_fldcw);
	register_emitter(op_ia32_FnstCW,        bemit_fnstcw);
	register_emitter(op_ia32_FtstFnstsw,    bemit_ftstfnstsw);
	register_emitter(op_ia32_FucomFnstsw,   bemit_fucomfnstsw);
	register_emitter(op_ia32_Fucomi,        bemit_fucomi);
	register_emitter(op_ia32_FucompFnstsw,  bemit_fucompfnstsw);
	register_emitter(op_ia32_Fucompi,       bemit_fucomip);
	register_emitter(op_ia32_FucomppFnstsw, bemit_fucomppfnstsw);
	register_emitter(op_ia32_IDiv,          bemit_idiv);
	register_emitter(op_ia32_IJmp,          bemit_ijmp);
	register_emitter(op_ia32_IMul,          bemit_imul);
	register_emitter(op_ia32_IMul1OP,       bemit_imul1op);
	register_emitter(op_ia32_Inc,           bemit_inc);
	register_emitter(op_ia32_IncMem,        bemit_incmem);
	register_emitter(op_ia32_Jcc,           bemit_ia32_jcc);
	register_emitter(op_ia32_Jmp,           bemit_jump);
	register_emitter(op_ia32_LdTls,         bemit_ldtls);
	register_emitter(op_ia32_Lea,           bemit_lea);
	register_emitter(op_ia32_Leave,         bemit_leave);
	register_emitter(op_ia32_Load,          bemit_load);
	register_emitter(op_ia32_Minus64Bit,    bemit_minus64bit);
	register_emitter(op_ia32_Mul,           bemit_mul);
	register_emitter(op_ia32_Neg,           bemit_neg);
	register_emitter(op_ia32_NegMem,        bemit_negmem);
	register_emitter(op_ia32_Not,           bemit_not);
	register_emitter(op_ia32_NotMem,        bemit_notmem);
	register_emitter(op_ia32_Or,            bemit_or);
	register_emitter(op_ia32_OrMem,         bemit_ormem);
	register_emitter(op_ia32_OrMem8Bit,     bemit_ormem8bit);
	register_emitter(op_ia32_Pop,           bemit_pop);
	register_emitter(op_ia32_PopEbp,        bemit_pop);
	register_emitter(op_ia32_PopMem,        bemit_popmem);
	register_emitter(op_ia32_Push,          bemit_push);
	register_emitter(op_ia32_RepPrefix,     bemit_rep);
	register_emitter(op_ia32_Rol,           bemit_rol);
	register_emitter(op_ia32_RolMem,        bemit_rolmem);
	register_emitter(op_ia32_Ror,           bemit_ror);
	register_emitter(op_ia32_RorMem,        bemit_rormem);
	register_emitter(op_ia32_Sahf,          bemit_sahf);
	register_emitter(op_ia32_Sar,           bemit_sar);
	register_emitter(op_ia32_SarMem,        bemit_sarmem);
	register_emitter(op_ia32_Sbb,           bemit_sbb);
	register_emitter(op_ia32_Setcc,         bemit_setcc);
	register_emitter(op_ia32_Shl,           bemit_shl);
	register_emitter(op_ia32_ShlD,          bemit_shld);
	register_emitter(op_ia32_ShlMem,        bemit_shlmem);
	register_emitter(op_ia32_Shr,           bemit_shr);
	register_emitter(op_ia32_ShrD,          bemit_shrd);
	register_emitter(op_ia32_ShrMem,        bemit_shrmem);
	register_emitter(op_ia32_Stc,           bemit_stc);
	register_emitter(op_ia32_Store,         bemit_store);
	register_emitter(op_ia32_Store8Bit,     bemit_store);
	register_emitter(op_ia32_Sub,           bemit_sub);
	register_emitter(op_ia32_SubMem,        bemit_submem);
	register_emitter(op_ia32_SubMem8Bit,    bemit_submem8bit);
	register_emitter(op_ia32_SubSP,         bemit_subsp);
	register_emitter(op_ia32_SwitchJmp,     bemit_switchjmp);
	register_emitter(op_ia32_Test,          bemit_test);
	register_emitter(op_ia32_Test8Bit,      bemit_test8bit);
	register_emitter(op_ia32_Xor,           bemit_xor);
	register_emitter(op_ia32_Xor0,          bemit_xor0);
	register_emitter(op_ia32_XorMem,        bemit_xormem);
	register_emitter(op_ia32_XorMem8Bit,    bemit_xormem8bit);
	register_emitter(op_ia32_fabs,          bemit_fabs);
	register_emitter(op_ia32_fadd,          bemit_fadd);
	register_emitter(op_ia32_faddp,         bemit_faddp);
	register_emitter(op_ia32_fchs,          bemit_fchs);
	register_emitter(op_ia32_fdiv,          bemit_fdiv);
	register_emitter(op_ia32_fdivp,         bemit_fdivp);
	register_emitter(op_ia32_fdivr,         bemit_fdivr);
	register_emitter(op_ia32_fdivrp,        bemit_fdivrp);
	register_emitter(op_ia32_fild,          bemit_fild);
	register_emitter(op_ia32_fist,          bemit_fist);
	register_emitter(op_ia32_fistp,         bemit_fistp);
	register_emitter(op_ia32_fld,           bemit_fld);
	register_emitter(op_ia32_fld1,          bemit_fld1);
	register_emitter(op_ia32_fldz,          bemit_fldz);
	register_emitter(op_ia32_fmul,          bemit_fmul);
	register_emitter(op_ia32_fmulp,         bemit_fmulp);
	register_emitter(op_ia32_fpop,          bemit_fpop);
	register_emitter(op_ia32_fpush,         bemit_fpush);
	register_emitter(op_ia32_fpushCopy,     bemit_fpushcopy);
	register_emitter(op_ia32_fst,           bemit_fst);
	register_emitter(op_ia32_fstp,          bemit_fstp);
	register_emitter(op_ia32_fsub,          bemit_fsub);
	register_emitter(op_ia32_fsubp,         bemit_fsubp);
	register_emitter(op_ia32_fsubr,         bemit_fsubr);
	register_emitter(op_ia32_fsubrp,        bemit_fsubrp);
	register_emitter(op_ia32_fxch,          bemit_fxch);

	/* ignore the following nodes */
	register_emitter(op_ia32_ProduceVal,   emit_Nothing);
	register_emitter(op_be_Barrier,        emit_Nothing);
	register_emitter(op_be_Keep,           emit_Nothing);
	register_emitter(op_be_Start,          emit_Nothing);
	register_emitter(op_Phi,               emit_Nothing);
	register_emitter(op_Start,             emit_Nothing);
}

static void gen_binary_block(ir_node *block)
{
	ir_node *node;

	ia32_emit_block_header(block);

	/* emit the contents of the block */
	sched_foreach(block, node) {
		ia32_emit_node(node);
	}
}

void ia32_gen_binary_routine(ir_graph *irg)
{
	ir_entity        *entity    = get_irg_entity(irg);
	const arch_env_t *arch_env  = be_get_irg_arch_env(irg);
	ia32_irg_data_t  *irg_data  = ia32_get_irg_data(irg);
	ir_node         **blk_sched = irg_data->blk_sched;
	int i, n;

	isa = (ia32_isa_t*) arch_env;

	ia32_register_binary_emitters();

	be_gas_emit_function_prolog(entity, ia32_cg_config.function_alignment);

	/* we use links to point to target blocks */
	ir_reserve_resources(irg, IR_RESOURCE_IRN_LINK);
	irg_block_walk_graph(irg, ia32_gen_labels, NULL, NULL);

	/* initialize next block links */
	n = ARR_LEN(blk_sched);
	for (i = 0; i < n; ++i) {
		ir_node *block = blk_sched[i];
		ir_node *prev  = i > 0 ? blk_sched[i-1] : NULL;

		set_irn_link(block, prev);
	}

	for (i = 0; i < n; ++i) {
		ir_node *block = blk_sched[i];
		gen_binary_block(block);
	}

	be_gas_emit_function_epilog(entity);
	be_dbg_method_end();
	be_emit_char('\n');
	be_emit_write_line();

	ir_free_resources(irg, IR_RESOURCE_IRN_LINK);
}


void ia32_init_emitter(void)
{
	lc_opt_entry_t *be_grp;
	lc_opt_entry_t *ia32_grp;

	be_grp   = lc_opt_get_grp(firm_opt_get_root(), "be");
	ia32_grp = lc_opt_get_grp(be_grp, "ia32");

	lc_opt_add_table(ia32_grp, ia32_emitter_options);

	build_reg_map();

	FIRM_DBG_REGISTER(dbg, "firm.be.ia32.emitter");
}
