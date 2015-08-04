/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief   arm emitter
 * @author  Oliver Richter, Tobias Gneist, Michael Beck, Matthias Braun
 */
#include <inttypes.h>

#include "arm_cconv.h"
#include "arm_emitter.h"
#include "arm_new_nodes.h"
#include "be_t.h"
#include "bearch_arm_t.h"
#include "beblocksched.h"
#include "bediagnostic.h"
#include "begnuas.h"
#include "benode.h"
#include "besched.h"
#include "debug.h"
#include "gen_arm_emitter.h"
#include "gen_arm_regalloc_if.h"
#include "irgwalk.h"
#include "panic.h"
#include "pmap.h"
#include "util.h"

/** An entry in the ent_or_tv set. */
typedef struct ent_or_tv_t ent_or_tv_t;
struct ent_or_tv_t {
	union {
		ir_entity  *entity;  /**< An entity. */
		ir_tarval  *tv;      /**< A tarval. */
		const void *generic; /**< For generic compare. */
	} u;
	unsigned     label;      /**< the associated label. */
	bool         is_entity;  /**< true if an entity is stored. */
	ent_or_tv_t *next;       /**< next in list. */
};

DEBUG_ONLY(static firm_dbg_module_t *dbg = NULL;)

static struct obstack obst;
static pmap          *ent_or_tv;
static ent_or_tv_t   *ent_or_tv_first;
static ent_or_tv_t  **ent_or_tv_anchor;

static void arm_emit_register(const arch_register_t *reg)
{
	be_emit_string(reg->name);
}

static void arm_emit_source_register(const ir_node *node, int pos)
{
	const arch_register_t *reg = arch_get_irn_register_in(node, pos);
	arm_emit_register(reg);
}

static void arm_emit_dest_register(const ir_node *node, int pos)
{
	const arch_register_t *reg = arch_get_irn_register_out(node, pos);
	arm_emit_register(reg);
}

static void arm_emit_offset(const ir_node *node)
{
	const arm_load_store_attr_t *attr = get_arm_load_store_attr_const(node);
	assert(attr->base.is_load_store);

	be_emit_irprintf("0x%X", attr->offset);
}

/**
 * Emit the arm fpa instruction suffix depending on the mode.
 */
static void arm_emit_fpa_postfix(const ir_mode *mode)
{
	int bits = get_mode_size_bits(mode);
	char c = 'e';

	if (bits == 32)
		c = 's';
	else if (bits == 64)
		c = 'd';
	be_emit_char(c);
}

static void arm_emit_float_load_store_mode(const ir_node *node)
{
	const arm_load_store_attr_t *attr = get_arm_load_store_attr_const(node);
	arm_emit_fpa_postfix(attr->load_store_mode);
}

static void arm_emit_float_arithmetic_mode(const ir_node *node)
{
	const arm_farith_attr_t *attr = get_arm_farith_attr_const(node);
	arm_emit_fpa_postfix(attr->mode);
}

static void arm_emit_address(const ir_node *node)
{
	const arm_Address_attr_t *address = get_arm_Address_attr_const(node);
	ir_entity                *entity  = address->entity;

	be_gas_emit_entity(entity);

	/* TODO do something with offset */
}

static void arm_emit_load_mode(const ir_node *node)
{
	const arm_load_store_attr_t *attr = get_arm_load_store_attr_const(node);
	ir_mode *mode      = attr->load_store_mode;
	int      bits      = get_mode_size_bits(mode);
	bool     is_signed = mode_is_signed(mode);
	if (bits == 16) {
		be_emit_string(is_signed ? "sh" : "h");
	} else if (bits == 8) {
		be_emit_string(is_signed ? "sb" : "b");
	} else {
		assert(bits == 32);
	}
}

static void arm_emit_store_mode(const ir_node *node)
{
	const arm_load_store_attr_t *attr = get_arm_load_store_attr_const(node);
	ir_mode *mode      = attr->load_store_mode;
	int      bits      = get_mode_size_bits(mode);
	if (bits == 16) {
		be_emit_cstring("h");
	} else if (bits == 8) {
		be_emit_cstring("b");
	} else {
		assert(bits == 32);
	}
}

static char const *get_shf_mod_name(arm_shift_modifier_t mod)
{
	switch (mod) {
	case ARM_SHF_ASR_REG:
	case ARM_SHF_ASR_IMM: return "asr";
	case ARM_SHF_LSL_REG:
	case ARM_SHF_LSL_IMM: return "lsl";
	case ARM_SHF_LSR_REG:
	case ARM_SHF_LSR_IMM: return "lsr";
	case ARM_SHF_ROR_REG:
	case ARM_SHF_ROR_IMM: return "ror";
	default:
		break;
	}
	panic("can't emit this shf_mod_name %d", (int) mod);
}

static void arm_emit_shifter_operand(const ir_node *node)
{
	const arm_shifter_operand_t *attr = get_arm_shifter_operand_attr_const(node);

	switch (attr->shift_modifier) {
	case ARM_SHF_REG:
		arm_emit_source_register(node, attr->shifter_op_input);
		return;
	case ARM_SHF_IMM: {
		unsigned val = attr->immediate_value;
		val = (val >> attr->shift_immediate)
			| (val << ((32-attr->shift_immediate) & 31));
		val &= 0xFFFFFFFF;
		be_emit_irprintf("#0x%X", val);
		return;
	}
	case ARM_SHF_ASR_IMM:
	case ARM_SHF_LSL_IMM:
	case ARM_SHF_LSR_IMM:
	case ARM_SHF_ROR_IMM: {
		arm_emit_source_register(node, attr->shifter_op_input);
		char const *const mod = get_shf_mod_name(attr->shift_modifier);
		be_emit_irprintf(", %s #%u", mod, attr->shift_immediate);
		return;
	}

	case ARM_SHF_ASR_REG:
	case ARM_SHF_LSL_REG:
	case ARM_SHF_LSR_REG:
	case ARM_SHF_ROR_REG: {
		arm_emit_source_register(node, attr->shifter_op_input);
		char const *const mod = get_shf_mod_name(attr->shift_modifier);
		be_emit_irprintf(", %s ", mod);
		arm_emit_source_register(node, attr->shifter_op_input+1);
		return;
	}

	case ARM_SHF_RRX:
		arm_emit_source_register(node, attr->shifter_op_input);
		panic("RRX shifter emitter TODO");

	case ARM_SHF_INVALID:
		break;
	}
	panic("invalid shift_modifier while emitting %+F", node);
}

/**
 * Returns a unique label. This number will not be used a second time.
 */
static unsigned get_unique_label(void)
{
	static unsigned id = 0;
	return ++id;
}

static void emit_constant_name(const ent_or_tv_t *entry)
{
	be_emit_irprintf("%sC%u", be_gas_get_private_prefix(), entry->label);
}

/**
 * Returns the target block for a control flow node.
 */
static ir_node *get_cfop_target_block(const ir_node *irn)
{
	return (ir_node*)get_irn_link(irn);
}

/**
 * Emit the target label for a control flow node.
 */
static void arm_emit_cfop_target(const ir_node *irn)
{
	ir_node *block = get_cfop_target_block(irn);
	be_gas_emit_block_name(block);
}

void arm_emitf(const ir_node *node, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	be_emit_char('\t');
	for (;;) {
		const char *start = format;
		while (*format != '%' && *format != '\n'  && *format != '\0')
			++format;
		be_emit_string_len(start, format - start);

		if (*format == '\0')
			break;

		if (*format == '\n') {
			++format;
			be_emit_char('\n');
			be_emit_write_line();
			be_emit_char('\t');
			continue;
		}

		++format;

		switch (*format++) {
		case '%':
			be_emit_char('%');
			break;

		case 'S': {
			if (!is_digit(*format))
				goto unknown;
			unsigned const pos = *format++ - '0';
			arm_emit_source_register(node, pos);
			break;
		}

		case 'D': {
			if (!is_digit(*format))
				goto unknown;
			unsigned const pos = *format++ - '0';
			arm_emit_dest_register(node, pos);
			break;
		}

		case 'I':
			arm_emit_address(node);
			break;

		case 'o':
			arm_emit_offset(node);
			break;

		case 'O':
			arm_emit_shifter_operand(node);
			break;

		case 'C': {
			const ent_or_tv_t *name = va_arg(ap, const ent_or_tv_t*);
			emit_constant_name(name);
			break;
		}

		case 'm': {
			ir_mode *mode = va_arg(ap, ir_mode*);
			arm_emit_fpa_postfix(mode);
			break;
		}

		case 'M':
			switch (*format++) {
			case 'L': arm_emit_load_mode(node);             break;
			case 'S': arm_emit_store_mode(node);            break;
			case 'A': arm_emit_float_arithmetic_mode(node); break;
			case 'F': arm_emit_float_load_store_mode(node); break;
			default:
				--format;
				goto unknown;
			}
			break;

		case 'X': {
			int num = va_arg(ap, int);
			be_emit_irprintf("%X", num);
			break;
		}

		case 'u': {
			unsigned num = va_arg(ap, unsigned);
			be_emit_irprintf("%u", num);
			break;
		}

		case 'd': {
			int num = va_arg(ap, int);
			be_emit_irprintf("%d", num);
			break;
		}

		case 's': {
			const char *string = va_arg(ap, const char *);
			be_emit_string(string);
			break;
		}

		case 'r': {
			arch_register_t *reg = va_arg(ap, arch_register_t*);
			arm_emit_register(reg);
			break;
		}

		case 't': {
			const ir_node *n = va_arg(ap, const ir_node*);
			arm_emit_cfop_target(n);
			break;
		}

		default:
unknown:
			panic("unknown format conversion");
		}
	}
	va_end(ap);
	be_emit_finish_line_gas(node);
}

static ent_or_tv_t *get_ent_or_tv_entry(const ent_or_tv_t *key)
{
	ent_or_tv_t *entry = pmap_get(ent_or_tv_t, ent_or_tv, key->u.generic);
	if (entry == NULL) {
		entry = OALLOC(&obst, ent_or_tv_t);
		*entry = *key;
		entry->label = get_unique_label();
		entry->next  = NULL;
		*ent_or_tv_anchor = entry;
		ent_or_tv_anchor  = &entry->next;
		pmap_insert(ent_or_tv, key->u.generic, entry);
	}
	return entry;
}

/**
 * Emit an Address.
 */
static void emit_arm_Address(const ir_node *irn)
{
	const arm_Address_attr_t *attr = get_arm_Address_attr_const(irn);
	ent_or_tv_t key;
	key.u.entity  = attr->entity;
	key.is_entity = true;
	key.label     = 0;
	ent_or_tv_t *entry = get_ent_or_tv_entry(&key);

	/* load the symbol indirect */
	arm_emitf(irn, "ldr %D0, %C", entry);
}

static void emit_arm_FrameAddr(const ir_node *irn)
{
	const arm_Address_attr_t *attr = get_arm_Address_attr_const(irn);
	arm_emitf(irn, "add %D0, %S0, #0x%X", attr->fp_offset);
}

/**
 * Emit a floating point fpa constant.
 */
static void emit_arm_fConst(const ir_node *irn)
{
	ir_tarval *const tv = get_fConst_value(irn);
	ent_or_tv_t key = {
		.u.tv      = tv,
		.is_entity = false
	};
	ent_or_tv_t *entry = get_ent_or_tv_entry(&key);

	/* load the tarval indirect */
	ir_mode *const mode = get_tarval_mode(tv);
	arm_emitf(irn, "ldf%m %D0, %C", mode, entry);
}

/**
 * Returns the next block in a block schedule.
 */
static ir_node *sched_next_block(const ir_node *block)
{
    return (ir_node*)get_irn_link(block);
}

/**
 * Emit a Compare with conditional branch.
 */
static void emit_arm_B(const ir_node *irn)
{
	const ir_node *proj_true  = NULL;
	const ir_node *proj_false = NULL;
	foreach_out_edge(irn, edge) {
		ir_node *proj = get_edge_src_irn(edge);
		unsigned nr   = get_Proj_num(proj);
		if (nr == pn_Cond_true) {
			proj_true = proj;
		} else {
			proj_false = proj;
		}
	}

	ir_node *const op1 = get_irn_n(irn, n_arm_B_flags);
	assert(is_arm_Cmn(op1) || is_arm_Cmp(op1) || is_arm_Tst(op1));

	arm_cmp_attr_t const *const cmp_attr = get_arm_cmp_attr_const(op1);

	ir_relation relation = get_arm_CondJmp_relation(irn);
	if (cmp_attr->ins_permuted)
		relation = get_inversed_relation(relation);

	/* for now, the code works for scheduled and non-schedules blocks */
	const ir_node *block      = get_nodes_block(irn);
	const ir_node *next_block = sched_next_block(block);

	assert(relation != ir_relation_false);
	assert(relation != ir_relation_true);

	if (get_cfop_target_block(proj_true) == next_block) {
		/* exchange both proj's so the second one can be omitted */
		const ir_node *t = proj_true;

		proj_true  = proj_false;
		proj_false = t;
		relation   = get_negated_relation(relation);
	}

	char const *suffix;
	bool const  is_signed = !cmp_attr->is_unsigned;
	switch (relation & (ir_relation_less_equal_greater)) {
		case ir_relation_equal:         suffix = "eq"; break;
		case ir_relation_less:          suffix = is_signed ? "lt" : "lo"; break;
		case ir_relation_less_equal:    suffix = is_signed ? "le" : "ls"; break;
		case ir_relation_greater:       suffix = is_signed ? "gt" : "hi"; break;
		case ir_relation_greater_equal: suffix = is_signed ? "ge" : "hs"; break;
		case ir_relation_less_greater:  suffix = "ne"; break;
		case ir_relation_less_equal_greater: suffix = "al"; break;
		default: panic("Cmp has unsupported relation");
	}

	/* emit the true proj */
	arm_emitf(irn, "b%s %t", suffix, proj_true);

	if (get_cfop_target_block(proj_false) == next_block) {
		if (be_options.verbose_asm) {
			arm_emitf(irn, "/* fallthrough to %t */", proj_false);
		}
	} else {
		arm_emitf(irn, "b %t", proj_false);
	}
}

static void emit_arm_SwitchJmp(const ir_node *irn)
{
	const arm_SwitchJmp_attr_t *attr = get_arm_SwitchJmp_attr_const(irn);
	arm_emitf(irn, "ldrls pc, [pc, %S0, asl #2]");

	be_emit_jump_table(irn, attr->table, NULL, get_cfop_target_block);
}

/** Emit an IncSP node */
static void emit_be_IncSP(const ir_node *irn)
{
	int offs = -be_get_IncSP_offset(irn);
	if (offs == 0)
		return;

	const char *op = "add";
	if (offs < 0) {
		op   = "sub";
		offs = -offs;
	}
	arm_emitf(irn, "%s %D0, %S0, #0x%X", op, offs);
}

static void emit_be_Copy(const ir_node *irn)
{
	arch_register_t const *const out = arch_get_irn_register_out(irn, 0);
	if (arch_get_irn_register_in(irn, 0) == out) {
		/* omitted Copy */
		return;
	}

	arch_register_class_t const *const cls = out->cls;
	if (cls == &arm_reg_classes[CLASS_arm_gp]) {
		arm_emitf(irn, "mov %D0, %S0");
	} else if (cls == &arm_reg_classes[CLASS_arm_fpa]) {
		arm_emitf(irn, "mvf %D0, %S0");
	} else {
		panic("move not supported for this register class");
	}
}

static void emit_be_Perm(const ir_node *irn)
{
	arm_emitf(irn,
		"eor %D0, %D0, %D1\n"
		"eor %D1, %D0, %D1\n"
		"eor %D0, %D0, %D1");
}

static void emit_be_MemPerm(const ir_node *node)
{
	/* TODO: this implementation is slower than necessary.
	   The longterm goal is however to avoid the memperm node completely */

	int memperm_arity = be_get_MemPerm_entity_arity(node);
	if (memperm_arity > 12)
		panic("memperm with more than 12 inputs not supported yet");

	int sp_change = 0;
	for (int i = 0; i < memperm_arity; ++i) {
		/* spill register */
		arm_emitf(node, "str r%d, [sp, #-4]!", i);
		sp_change += 4;
		/* load from entity */
		ir_entity *entity = be_get_MemPerm_in_entity(node, i);
		int        offset = get_entity_offset(entity) + sp_change;
		arm_emitf(node, "ldr r%d, [sp, #%d]", i, offset);
	}

	for (int i = memperm_arity; i-- > 0; ) {
		/* store to new entity */
		ir_entity *entity = be_get_MemPerm_out_entity(node, i);
		int        offset = get_entity_offset(entity) + sp_change;
		arm_emitf(node, "str r%d, [sp, #%d]", i, offset);
		/* restore register */
		arm_emitf(node, "ldr r%d, [sp], #4", i);
		sp_change -= 4;
	}
	assert(sp_change == 0);
}

static void emit_arm_Jmp(const ir_node *node)
{
	/* for now, the code works for scheduled and non-schedules blocks */
	const ir_node *block      = get_nodes_block(node);
	const ir_node *next_block = sched_next_block(block);
	if (get_cfop_target_block(node) != next_block) {
		arm_emitf(node, "b %t", node);
	} else {
		if (be_options.verbose_asm) {
			arm_emitf(node, "/* fallthrough to %t */", node);
		}
	}
}

/**
 * Enters the emitter functions for handled nodes into the generic
 * pointer of an opcode.
 */
static void arm_register_emitters(void)
{
	be_init_emitters();

	/* register all emitter functions defined in spec */
	arm_register_spec_emitters();

	/* custom emitter */
	be_set_emitter(op_arm_Address,   emit_arm_Address);
	be_set_emitter(op_arm_B,         emit_arm_B);
	be_set_emitter(op_arm_fConst,    emit_arm_fConst);
	be_set_emitter(op_arm_FrameAddr, emit_arm_FrameAddr);
	be_set_emitter(op_arm_Jmp,       emit_arm_Jmp);
	be_set_emitter(op_arm_SwitchJmp, emit_arm_SwitchJmp);
	be_set_emitter(op_be_Copy,       emit_be_Copy);
	be_set_emitter(op_be_CopyKeep,   emit_be_Copy);
	be_set_emitter(op_be_IncSP,      emit_be_IncSP);
	be_set_emitter(op_be_MemPerm,    emit_be_MemPerm);
	be_set_emitter(op_be_Perm,       emit_be_Perm);
}

/**
 * emit the block label if needed.
 */
static void arm_emit_block_header(ir_node *block, ir_node *prev)
{
	int  n_cfgpreds = get_Block_n_cfgpreds(block);
	bool need_label;
	if (n_cfgpreds == 1) {
		const ir_node *pred       = get_Block_cfgpred(block, 0);
		const ir_node *pred_block = get_nodes_block(pred);

		/* we don't need labels for fallthrough blocks, however switch-jmps
		 * are no fallthroughs */
		need_label =
			pred_block != prev ||
			(is_Proj(pred) && is_arm_SwitchJmp(get_Proj_pred(pred)));
	} else {
		need_label = true;
	}

	be_gas_begin_block(block, need_label);
}

/**
 * Walks over the nodes in a block connected by scheduling edges
 * and emits code for each node.
 */
static void arm_gen_block(ir_node *block, ir_node *prev_block)
{
	arm_emit_block_header(block, prev_block);
	be_dwarf_location(get_irn_dbg_info(block));
	sched_foreach(block, irn) {
		be_emit_node(irn);
	}
}

/**
 * Block-walker:
 * Sets labels for control flow nodes (jump target)
 */
static void arm_gen_labels(ir_node *block, void *env)
{
	(void)env;
	for (int n = get_Block_n_cfgpreds(block); n-- > 0; ) {
		ir_node *pred = get_Block_cfgpred(block, n);
		set_irn_link(pred, block);
	}
}

static parameter_dbg_info_t *construct_parameter_infos(ir_graph *irg)
{
	ir_entity            *entity   = get_irg_entity(irg);
	ir_type              *type     = get_entity_type(entity);
	calling_convention_t *cconv    = arm_decide_calling_convention(NULL, type);
	size_t                n_params = get_method_n_params(type);
	parameter_dbg_info_t *infos    = XMALLOCNZ(parameter_dbg_info_t, n_params);

	for (size_t i = 0; i < n_params; ++i) {
		const reg_or_stackslot_t *slot = &cconv->parameters[i];

		assert(infos[i].entity == NULL && infos[i].reg == NULL);
		if (slot->reg0 != NULL) {
			infos[i].reg = slot->reg0;
		} else {
			infos[i].entity = slot->entity;
		}
	}
	arm_free_calling_convention(cconv);

	return infos;
}

void arm_emit_function(ir_graph *irg)
{
	ent_or_tv = pmap_create();
	obstack_init(&obst);
	ent_or_tv_first  = NULL;
	ent_or_tv_anchor = &ent_or_tv_first;

	arm_register_emitters();

	/* create the block schedule */
	ir_node **blk_sched = be_create_block_schedule(irg);

	ir_entity            *const entity = get_irg_entity(irg);
	parameter_dbg_info_t *const infos  = construct_parameter_infos(irg);
	be_gas_emit_function_prolog(entity, 4, infos);

	ir_reserve_resources(irg, IR_RESOURCE_IRN_LINK);
	irg_block_walk_graph(irg, arm_gen_labels, NULL, NULL);

	ir_node *last_block = NULL;
	for (size_t i = 0, n = ARR_LEN(blk_sched); i < n;) {
		ir_node *block   = blk_sched[i++];
		ir_node *next_bl = i < n ? blk_sched[i] : NULL;

		/* set here the link. the emitter expects to find the next block here */
		set_irn_link(block, next_bl);
		arm_gen_block(block, last_block);
		last_block = block;
	}
	ir_free_resources(irg, IR_RESOURCE_IRN_LINK);

	/* emit entity and tarval values */
	if (ent_or_tv_first != NULL) {
		be_emit_cstring("\t.align 2\n");

		for (ent_or_tv_t *entry = ent_or_tv_first; entry; entry = entry->next) {
			emit_constant_name(entry);
			be_emit_cstring(":\n");
			be_emit_write_line();

			if (entry->is_entity) {
				be_emit_cstring("\t.word\t");
				be_gas_emit_entity(entry->u.entity);
				be_emit_char('\n');
				be_emit_write_line();
			} else {
				ir_tarval *tv   = entry->u.tv;
				unsigned   size = get_mode_size_bytes(get_tarval_mode(tv));

				/* beware: ARM fpa uses big endian format */
				for (unsigned vi = round_up2(size, 4); vi != 0;) {
					/* get 32 bits */
					uint32_t v;
					v  = get_tarval_sub_bits(tv, --vi) << 24;
					v |= get_tarval_sub_bits(tv, --vi) << 16;
					v |= get_tarval_sub_bits(tv, --vi) <<  8;
					v |= get_tarval_sub_bits(tv, --vi) <<  0;
					be_emit_irprintf("\t.word\t%" PRIu32 "\n", v);
					be_emit_write_line();
				}
			}
		}
		be_emit_char('\n');
		be_emit_write_line();
	}
	pmap_destroy(ent_or_tv);
	obstack_free(&obst, NULL);

	be_gas_emit_function_epilog(entity);
}

static const char *get_variant_string(arm_variant_t variant)
{
	switch (variant) {
	case ARM_VARIANT_4:   return "armv4";
	case ARM_VARIANT_5T:  return "armv5t";
	case ARM_VARIANT_6:   return "armv6";
	case ARM_VARIANT_6T2: return "armv6t2";
	case ARM_VARIANT_7:   return "armv7";
	}
	panic("invalid arm variant");
}

void arm_emit_file_prologue(void)
{
	be_emit_irprintf("\t.arch %s\n", get_variant_string(arm_cg_config.variant));
	be_emit_write_line();
	be_emit_cstring("\t.fpu softvfp\n");
	be_emit_write_line();
}

void arm_init_emitter(void)
{
	FIRM_DBG_REGISTER(dbg, "firm.be.arm.emit");
}
