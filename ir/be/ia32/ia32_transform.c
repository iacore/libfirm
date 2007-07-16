/*
 * Copyright (C) 1995-2007 University of Karlsruhe.  All right reserved.
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
 * @brief       This file implements the IR transformation from firm into ia32-Firm.
 * @author      Christian Wuerdig, Matthias Braun
 * @version     $Id$
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <limits.h>

#include "irargs_t.h"
#include "irnode_t.h"
#include "irgraph_t.h"
#include "irmode_t.h"
#include "iropt_t.h"
#include "irop_t.h"
#include "irprog_t.h"
#include "iredges_t.h"
#include "irgmod.h"
#include "irvrfy.h"
#include "ircons.h"
#include "irgwalk.h"
#include "irprintf.h"
#include "debug.h"
#include "irdom.h"
#include "archop.h"
#include "error.h"

#include "../benode_t.h"
#include "../besched.h"
#include "../beabi.h"
#include "../beutil.h"
#include "../beirg_t.h"
#include "../betranshlp.h"

#include "bearch_ia32_t.h"
#include "ia32_nodes_attr.h"
#include "ia32_transform.h"
#include "ia32_new_nodes.h"
#include "ia32_map_regs.h"
#include "ia32_dbg_stat.h"
#include "ia32_optimize.h"
#include "ia32_util.h"

#include "gen_ia32_regalloc_if.h"

#define SFP_SIGN "0x80000000"
#define DFP_SIGN "0x8000000000000000"
#define SFP_ABS  "0x7FFFFFFF"
#define DFP_ABS  "0x7FFFFFFFFFFFFFFF"

#define TP_SFP_SIGN "ia32_sfp_sign"
#define TP_DFP_SIGN "ia32_dfp_sign"
#define TP_SFP_ABS  "ia32_sfp_abs"
#define TP_DFP_ABS  "ia32_dfp_abs"

#define ENT_SFP_SIGN "IA32_SFP_SIGN"
#define ENT_DFP_SIGN "IA32_DFP_SIGN"
#define ENT_SFP_ABS  "IA32_SFP_ABS"
#define ENT_DFP_ABS  "IA32_DFP_ABS"

#define mode_vfp	(ia32_reg_classes[CLASS_ia32_vfp].mode)
#define mode_xmm    (ia32_reg_classes[CLASS_ia32_xmm].mode)

DEBUG_ONLY(static firm_dbg_module_t *dbg = NULL;)

/** hold the current code generator during transformation */
static ia32_code_gen_t *env_cg = NULL;

extern ir_op *get_op_Mulh(void);

typedef ir_node *construct_binop_func(dbg_info *db, ir_graph *irg,
        ir_node *block, ir_node *base, ir_node *index, ir_node *op1,
        ir_node *op2, ir_node *mem);

typedef ir_node *construct_binop_float_func(dbg_info *db, ir_graph *irg,
        ir_node *block, ir_node *base, ir_node *index, ir_node *op1,
        ir_node *op2, ir_node *mem, ir_node *fpcw);

typedef ir_node *construct_unop_func(dbg_info *db, ir_graph *irg,
        ir_node *block, ir_node *base, ir_node *index, ir_node *op,
        ir_node *mem);

/****************************************************************************************************
 *                  _        _                        __                           _   _
 *                 | |      | |                      / _|                         | | (_)
 *  _ __   ___   __| | ___  | |_ _ __ __ _ _ __  ___| |_ ___  _ __ _ __ ___   __ _| |_ _  ___  _ __
 * | '_ \ / _ \ / _` |/ _ \ | __| '__/ _` | '_ \/ __|  _/ _ \| '__| '_ ` _ \ / _` | __| |/ _ \| '_ \
 * | | | | (_) | (_| |  __/ | |_| | | (_| | | | \__ \ || (_) | |  | | | | | | (_| | |_| | (_) | | | |
 * |_| |_|\___/ \__,_|\___|  \__|_|  \__,_|_| |_|___/_| \___/|_|  |_| |_| |_|\__,_|\__|_|\___/|_| |_|
 *
 ****************************************************************************************************/

static ir_node *try_create_Immediate(ir_node *node,
                                     char immediate_constraint_type);

static ir_node *create_immediate_or_transform(ir_node *node,
                                              char immediate_constraint_type);

/**
 * Return true if a mode can be stored in the GP register set
 */
static INLINE int mode_needs_gp_reg(ir_mode *mode) {
	if(mode == mode_fpcw)
		return 0;
	return mode_is_int(mode) || mode_is_reference(mode) || mode == mode_b;
}

/**
 * Returns 1 if irn is a Const representing 0, 0 otherwise
 */
static INLINE int is_ia32_Const_0(ir_node *irn) {
	return is_ia32_irn(irn) && is_ia32_Const(irn) && get_ia32_immop_type(irn) == ia32_ImmConst
	       && tarval_is_null(get_ia32_Immop_tarval(irn));
}

/**
 * Returns 1 if irn is a Const representing 1, 0 otherwise
 */
static INLINE int is_ia32_Const_1(ir_node *irn) {
	return is_ia32_irn(irn) && is_ia32_Const(irn) && get_ia32_immop_type(irn) == ia32_ImmConst
	       && tarval_is_one(get_ia32_Immop_tarval(irn));
}

/**
 * Collects all Projs of a node into the node array. Index is the projnum.
 * BEWARE: The caller has to assure the appropriate array size!
 */
static void ia32_collect_Projs(ir_node *irn, ir_node **projs, int size) {
	const ir_edge_t *edge;
	assert(get_irn_mode(irn) == mode_T && "need mode_T");

	memset(projs, 0, size * sizeof(projs[0]));

	foreach_out_edge(irn, edge) {
		ir_node *proj = get_edge_src_irn(edge);
		int proj_proj = get_Proj_proj(proj);
		assert(proj_proj < size);
		projs[proj_proj] = proj;
	}
}

/**
 * Renumbers the proj having pn_old in the array tp pn_new
 * and removes the proj from the array.
 */
static INLINE void ia32_renumber_Proj(ir_node **projs, long pn_old, long pn_new) {
	fprintf(stderr, "Warning: renumber_Proj used!\n");
	if (projs[pn_old]) {
		set_Proj_proj(projs[pn_old], pn_new);
		projs[pn_old] = NULL;
	}
}

/**
 * creates a unique ident by adding a number to a tag
 *
 * @param tag   the tag string, must contain a %d if a number
 *              should be added
 */
static ident *unique_id(const char *tag)
{
	static unsigned id = 0;
	char str[256];

	snprintf(str, sizeof(str), tag, ++id);
	return new_id_from_str(str);
}

/**
 * Get a primitive type for a mode.
 */
static ir_type *get_prim_type(pmap *types, ir_mode *mode)
{
	pmap_entry *e = pmap_find(types, mode);
	ir_type *res;

	if (! e) {
		char buf[64];
		snprintf(buf, sizeof(buf), "prim_type_%s", get_mode_name(mode));
		res = new_type_primitive(new_id_from_str(buf), mode);
		set_type_alignment_bytes(res, 16);
		pmap_insert(types, mode, res);
	}
	else
		res = e->value;
	return res;
}

/**
 * Get an entity that is initialized with a tarval
 */
static ir_entity *get_entity_for_tv(ia32_code_gen_t *cg, ir_node *cnst)
{
	tarval *tv    = get_Const_tarval(cnst);
	pmap_entry *e = pmap_find(cg->isa->tv_ent, tv);
	ir_entity *res;
	ir_graph *rem;

	if (! e) {
		ir_mode *mode = get_irn_mode(cnst);
		ir_type *tp = get_Const_type(cnst);
		if (tp == firm_unknown_type)
			tp = get_prim_type(cg->isa->types, mode);

		res = new_entity(get_glob_type(), unique_id(".LC%u"), tp);

		set_entity_ld_ident(res, get_entity_ident(res));
		set_entity_visibility(res, visibility_local);
		set_entity_variability(res, variability_constant);
		set_entity_allocation(res, allocation_static);

		 /* we create a new entity here: It's initialization must resist on the
		    const code irg */
		rem = current_ir_graph;
		current_ir_graph = get_const_code_irg();
		set_atomic_ent_value(res, new_Const_type(tv, tp));
		current_ir_graph = rem;

		pmap_insert(cg->isa->tv_ent, tv, res);
	} else {
		res = e->value;
	}

	return res;
}

static int is_Const_0(ir_node *node) {
	if(!is_Const(node))
		return 0;

	return classify_Const(node) == CNST_NULL;
}

static int is_Const_1(ir_node *node) {
	if(!is_Const(node))
		return 0;

	return classify_Const(node) == CNST_ONE;
}

/**
 * Transforms a Const.
 */
static ir_node *gen_Const(ir_node *node) {
	ir_graph        *irg   = current_ir_graph;
	ir_node         *old_block = get_nodes_block(node);
	ir_node         *block = be_transform_node(old_block);
	dbg_info        *dbgi  = get_irn_dbg_info(node);
	ir_mode         *mode  = get_irn_mode(node);

	if (mode_is_float(mode)) {
		ir_node   *res   = NULL;
		ir_node   *noreg = ia32_new_NoReg_gp(env_cg);
		ir_node   *nomem = new_NoMem();
		ir_node   *load;
		ir_entity *floatent;

		if (! USE_SSE2(env_cg)) {
			cnst_classify_t clss = classify_Const(node);

			if (clss == CNST_NULL) {
				load = new_rd_ia32_vfldz(dbgi, irg, block);
				res  = load;
			} else if (clss == CNST_ONE) {
				load = new_rd_ia32_vfld1(dbgi, irg, block);
				res  = load;
			} else {
				floatent = get_entity_for_tv(env_cg, node);

				load     = new_rd_ia32_vfld(dbgi, irg, block, noreg, noreg, nomem, mode);
				set_ia32_op_type(load, ia32_AddrModeS);
				set_ia32_am_flavour(load, ia32_am_N);
				set_ia32_am_sc(load, floatent);
				set_ia32_flags(load, get_ia32_flags(load) | arch_irn_flags_rematerializable);
				res = new_r_Proj(irg, block, load, mode_vfp, pn_ia32_vfld_res);
			}
			set_ia32_ls_mode(load, mode);
		} else {
			floatent = get_entity_for_tv(env_cg, node);

			load     = new_rd_ia32_xLoad(dbgi, irg, block, noreg, noreg, nomem);
			set_ia32_op_type(load, ia32_AddrModeS);
			set_ia32_am_flavour(load, ia32_am_N);
			set_ia32_am_sc(load, floatent);
			set_ia32_ls_mode(load, mode);
			set_ia32_flags(load, get_ia32_flags(load) | arch_irn_flags_rematerializable);

			res = new_r_Proj(irg, block, load, mode_xmm, pn_ia32_xLoad_res);
		}

		SET_IA32_ORIG_NODE(load, ia32_get_old_node_name(env_cg, node));

		/* Const Nodes before the initial IncSP are a bad idea, because
		 * they could be spilled and we have no SP ready at that point yet.
		 * So add a dependency to the initial frame pointer calculation to
		 * avoid that situation.
		 */
		if (get_irg_start_block(irg) == block) {
			add_irn_dep(load, get_irg_frame(irg));
		}

		SET_IA32_ORIG_NODE(load, ia32_get_old_node_name(env_cg, node));
		return res;
	} else {
		ir_node *cnst = new_rd_ia32_Const(dbgi, irg, block);

		/* see above */
		if (get_irg_start_block(irg) == block) {
			add_irn_dep(cnst, get_irg_frame(irg));
		}

		set_ia32_Const_attr(cnst, node);
		SET_IA32_ORIG_NODE(cnst, ia32_get_old_node_name(env_cg, node));
		return cnst;
	}

	assert(0);
	return new_r_Bad(irg);
}

/**
 * Transforms a SymConst.
 */
static ir_node *gen_SymConst(ir_node *node) {
	ir_graph *irg   = current_ir_graph;
	ir_node  *old_block = get_nodes_block(node);
	ir_node  *block = be_transform_node(old_block);
	dbg_info *dbgi  = get_irn_dbg_info(node);
	ir_mode  *mode  = get_irn_mode(node);
	ir_node  *cnst;

	if (mode_is_float(mode)) {
		if (USE_SSE2(env_cg))
			cnst = new_rd_ia32_xConst(dbgi, irg, block);
		else
			cnst = new_rd_ia32_vfConst(dbgi, irg, block);
		//set_ia32_ls_mode(cnst, mode);
		set_ia32_ls_mode(cnst, mode_E);
	} else {
		cnst = new_rd_ia32_Const(dbgi, irg, block);
	}

	/* Const Nodes before the initial IncSP are a bad idea, because
	 * they could be spilled and we have no SP ready at that point yet
	 */
	if (get_irg_start_block(irg) == block) {
		add_irn_dep(cnst, get_irg_frame(irg));
	}

	set_ia32_Const_attr(cnst, node);
	SET_IA32_ORIG_NODE(cnst, ia32_get_old_node_name(env_cg, node));

	return cnst;
}

/* Generates an entity for a known FP const (used for FP Neg + Abs) */
ir_entity *ia32_gen_fp_known_const(ia32_known_const_t kct) {
	static const struct {
		const char *tp_name;
		const char *ent_name;
		const char *cnst_str;
	} names [ia32_known_const_max] = {
		{ TP_SFP_SIGN, ENT_SFP_SIGN, SFP_SIGN },	/* ia32_SSIGN */
		{ TP_DFP_SIGN, ENT_DFP_SIGN, DFP_SIGN },	/* ia32_DSIGN */
		{ TP_SFP_ABS,  ENT_SFP_ABS,  SFP_ABS },		/* ia32_SABS */
		{ TP_DFP_ABS,  ENT_DFP_ABS,  DFP_ABS }		/* ia32_DABS */
	};
	static ir_entity *ent_cache[ia32_known_const_max];

	const char    *tp_name, *ent_name, *cnst_str;
	ir_type       *tp;
	ir_node       *cnst;
	ir_graph      *rem;
	ir_entity     *ent;
	tarval        *tv;
	ir_mode       *mode;

	ent_name = names[kct].ent_name;
	if (! ent_cache[kct]) {
		tp_name  = names[kct].tp_name;
		cnst_str = names[kct].cnst_str;

		mode = kct == ia32_SSIGN || kct == ia32_SABS ? mode_Iu : mode_Lu;
		//mode = mode_xmm;
		tv  = new_tarval_from_str(cnst_str, strlen(cnst_str), mode);
		tp  = new_type_primitive(new_id_from_str(tp_name), mode);
		ent = new_entity(get_glob_type(), new_id_from_str(ent_name), tp);

		set_entity_ld_ident(ent, get_entity_ident(ent));
		set_entity_visibility(ent, visibility_local);
		set_entity_variability(ent, variability_constant);
		set_entity_allocation(ent, allocation_static);

		/* we create a new entity here: It's initialization must resist on the
		    const code irg */
		rem = current_ir_graph;
		current_ir_graph = get_const_code_irg();
		cnst = new_Const(mode, tv);
		current_ir_graph = rem;

		set_atomic_ent_value(ent, cnst);

		/* cache the entry */
		ent_cache[kct] = ent;
	}

	return ent_cache[kct];
}

#ifndef NDEBUG
/**
 * Prints the old node name on cg obst and returns a pointer to it.
 */
const char *ia32_get_old_node_name(ia32_code_gen_t *cg, ir_node *irn) {
	ia32_isa_t *isa = (ia32_isa_t *)cg->arch_env->isa;

	lc_eoprintf(firm_get_arg_env(), isa->name_obst, "%+F", irn);
	obstack_1grow(isa->name_obst, 0);
 	return obstack_finish(isa->name_obst);
}
#endif /* NDEBUG */

/* determine if one operator is an Imm */
static ir_node *get_immediate_op(ir_node *op1, ir_node *op2) {
	if (op1) {
		return is_ia32_Cnst(op1) ? op1 : (is_ia32_Cnst(op2) ? op2 : NULL);
	} else {
		return is_ia32_Cnst(op2) ? op2 : NULL;
	}
}

/* determine if one operator is not an Imm */
static ir_node *get_expr_op(ir_node *op1, ir_node *op2) {
	return !is_ia32_Cnst(op1) ? op1 : (!is_ia32_Cnst(op2) ? op2 : NULL);
}

static void fold_immediate(ir_node *node, int in1, int in2) {
	ir_node *left;
	ir_node *right;

	if (!(env_cg->opt & IA32_OPT_IMMOPS))
		return;

	left = get_irn_n(node, in1);
	right = get_irn_n(node, in2);
	if (! is_ia32_Cnst(right) && is_ia32_Cnst(left)) {
		/* we can only set right operand to immediate */
		if(!is_ia32_commutative(node))
			return;
		/* exchange left/right */
		set_irn_n(node, in1, right);
		set_irn_n(node, in2, ia32_get_admissible_noreg(env_cg, node, in2));
		copy_ia32_Immop_attr(node, left);
	} else if(is_ia32_Cnst(right)) {
		set_irn_n(node, in2, ia32_get_admissible_noreg(env_cg, node, in2));
		copy_ia32_Immop_attr(node, right);
	} else {
		return;
	}

	clear_ia32_commutative(node);
	set_ia32_am_support(node, get_ia32_am_support(node) & ~ia32_am_Source,
	                    get_ia32_am_arity(node));
}

/**
 * Construct a standard binary operation, set AM and immediate if required.
 *
 * @param op1   The first operand
 * @param op2   The second operand
 * @param func  The node constructor function
 * @return The constructed ia32 node.
 */
static ir_node *gen_binop(ir_node *node, ir_node *op1, ir_node *op2,
                          construct_binop_func *func, int commutative)
{
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_graph *irg      = current_ir_graph;
	dbg_info *dbgi     = get_irn_dbg_info(node);
	ir_node  *noreg_gp = ia32_new_NoReg_gp(env_cg);
	ir_node  *nomem    = new_NoMem();
	ir_node  *new_node;

	ir_node *new_op1 = be_transform_node(op1);
	ir_node *new_op2 = create_immediate_or_transform(op2, 0);
	if (is_ia32_Immediate(new_op2)) {
		commutative = 0;
	}

	new_node = func(dbgi, irg, block, noreg_gp, noreg_gp, new_op1, new_op2, nomem);
	if (func == new_rd_ia32_IMul) {
		set_ia32_am_support(new_node, ia32_am_Source, ia32_am_binary);
	} else {
		set_ia32_am_support(new_node, ia32_am_Full, ia32_am_binary);
	}

	SET_IA32_ORIG_NODE(new_node, ia32_get_old_node_name(env_cg, node));
	if (commutative) {
		set_ia32_commutative(new_node);
	}

	return new_node;
}

/**
 * Construct a standard binary operation, set AM and immediate if required.
 *
 * @param op1   The first operand
 * @param op2   The second operand
 * @param func  The node constructor function
 * @return The constructed ia32 node.
 */
static ir_node *gen_binop_sse_float(ir_node *node, ir_node *op1, ir_node *op2,
                                    construct_binop_func *func)
{
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_node  *new_op1  = be_transform_node(op1);
	ir_node  *new_op2  = be_transform_node(op2);
	ir_node  *new_node = NULL;
	dbg_info *dbgi     = get_irn_dbg_info(node);
	ir_graph *irg      = current_ir_graph;
	ir_mode  *mode     = get_irn_mode(node);
	ir_node  *noreg_gp = ia32_new_NoReg_gp(env_cg);
	ir_node  *nomem    = new_NoMem();

	new_node = func(dbgi, irg, block, noreg_gp, noreg_gp, new_op1, new_op2,
	                nomem);
	set_ia32_am_support(new_node, ia32_am_Source, ia32_am_binary);
	if (is_op_commutative(get_irn_op(node))) {
		set_ia32_commutative(new_node);
	}
	set_ia32_ls_mode(new_node, mode);

	SET_IA32_ORIG_NODE(new_node, ia32_get_old_node_name(env_cg, node));

	return new_node;
}

/**
 * Construct a standard binary operation, set AM and immediate if required.
 *
 * @param op1   The first operand
 * @param op2   The second operand
 * @param func  The node constructor function
 * @return The constructed ia32 node.
 */
static ir_node *gen_binop_x87_float(ir_node *node, ir_node *op1, ir_node *op2,
                                    construct_binop_float_func *func)
{
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_node  *new_op1  = be_transform_node(op1);
	ir_node  *new_op2  = be_transform_node(op2);
	ir_node  *new_node = NULL;
	dbg_info *dbgi     = get_irn_dbg_info(node);
	ir_graph *irg      = current_ir_graph;
	ir_node  *noreg_gp = ia32_new_NoReg_gp(env_cg);
	ir_node  *nomem    = new_NoMem();
	ir_node  *fpcw     = be_abi_get_ignore_irn(env_cg->birg->abi,
	                                           &ia32_fp_cw_regs[REG_FPCW]);

	new_node = func(dbgi, irg, block, noreg_gp, noreg_gp, new_op1, new_op2,
	                nomem, fpcw);
	set_ia32_am_support(new_node, ia32_am_Source, ia32_am_binary);
	if (is_op_commutative(get_irn_op(node))) {
		set_ia32_commutative(new_node);
	}

	SET_IA32_ORIG_NODE(new_node, ia32_get_old_node_name(env_cg, node));

	return new_node;
}

/**
 * Construct a shift/rotate binary operation, sets AM and immediate if required.
 *
 * @param op1   The first operand
 * @param op2   The second operand
 * @param func  The node constructor function
 * @return The constructed ia32 node.
 */
static ir_node *gen_shift_binop(ir_node *node, ir_node *op1, ir_node *op2,
                                construct_binop_func *func)
{
	ir_node  *block   = be_transform_node(get_nodes_block(node));
	ir_node  *new_op1 = be_transform_node(op1);
	ir_node  *new_op2;
	ir_node  *new_op  = NULL;
	dbg_info *dbgi    = get_irn_dbg_info(node);
	ir_graph *irg     = current_ir_graph;
	ir_node  *noreg   = ia32_new_NoReg_gp(env_cg);
	ir_node  *nomem   = new_NoMem();

	assert(! mode_is_float(get_irn_mode(node))
	         && "Shift/Rotate with float not supported");

	new_op2 = create_immediate_or_transform(op2, 'N');

	new_op = func(dbgi, irg, block, noreg, noreg, new_op1, new_op2, nomem);

	/* set AM support */
	set_ia32_am_support(new_op, ia32_am_Dest, ia32_am_binary);

	SET_IA32_ORIG_NODE(new_op, ia32_get_old_node_name(env_cg, node));

	set_ia32_emit_cl(new_op);

	return new_op;
}


/**
 * Construct a standard unary operation, set AM and immediate if required.
 *
 * @param op    The operand
 * @param func  The node constructor function
 * @return The constructed ia32 node.
 */
static ir_node *gen_unop(ir_node *node, ir_node *op, construct_unop_func *func)
{
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_node  *new_op   = be_transform_node(op);
	ir_node  *new_node = NULL;
	ir_graph *irg      = current_ir_graph;
	dbg_info *dbgi     = get_irn_dbg_info(node);
	ir_node  *noreg    = ia32_new_NoReg_gp(env_cg);
	ir_node  *nomem    = new_NoMem();

	new_node = func(dbgi, irg, block, noreg, noreg, new_op, nomem);
	DB((dbg, LEVEL_1, "INT unop ..."));
	set_ia32_am_support(new_node, ia32_am_Dest, ia32_am_unary);

	SET_IA32_ORIG_NODE(new_node, ia32_get_old_node_name(env_cg, node));

	return new_node;
}

/**
 * Creates an ia32 Add.
 *
 * @return the created ia32 Add node
 */
static ir_node *gen_Add(ir_node *node) {
	ir_node  *block   = be_transform_node(get_nodes_block(node));
	ir_node  *op1     = get_Add_left(node);
	ir_node  *new_op1 = be_transform_node(op1);
	ir_node  *op2     = get_Add_right(node);
	ir_node  *new_op2 = be_transform_node(op2);
	ir_node  *new_op  = NULL;
	ir_graph *irg     = current_ir_graph;
	dbg_info *dbgi    = get_irn_dbg_info(node);
	ir_mode  *mode    = get_irn_mode(node);
	ir_node  *noreg   = ia32_new_NoReg_gp(env_cg);
	ir_node  *nomem   = new_NoMem();
	ir_node  *expr_op, *imm_op;

	/* Check if immediate optimization is on and */
	/* if it's an operation with immediate.      */
	imm_op  = (env_cg->opt & IA32_OPT_IMMOPS) ? get_immediate_op(new_op1, new_op2) : NULL;
	expr_op = get_expr_op(new_op1, new_op2);

	assert((expr_op || imm_op) && "invalid operands");

	if (mode_is_float(mode)) {
		if (USE_SSE2(env_cg))
			return gen_binop_sse_float(node, op1, op2, new_rd_ia32_xAdd);
		else
			return gen_binop_x87_float(node, op1, op2, new_rd_ia32_vfadd);
	}

	/* integer ADD */
	if (! expr_op) {
		ia32_immop_type_t tp1 = get_ia32_immop_type(new_op1);
		ia32_immop_type_t tp2 = get_ia32_immop_type(new_op2);

		/* No expr_op means, that we have two const - one symconst and */
		/* one tarval or another symconst - because this case is not   */
		/* covered by constant folding                                 */
		/* We need to check for:                                       */
		/*  1) symconst + const    -> becomes a LEA                    */
		/*  2) symconst + symconst -> becomes a const + LEA as the elf */
		/*        linker doesn't support two symconsts                 */

		if (tp1 == ia32_ImmSymConst && tp2 == ia32_ImmSymConst) {
			/* this is the 2nd case */
			new_op = new_rd_ia32_Lea(dbgi, irg, block, new_op1, noreg);
			set_ia32_am_sc(new_op, get_ia32_Immop_symconst(new_op2));
			set_ia32_am_flavour(new_op, ia32_am_B);
			set_ia32_op_type(new_op, ia32_AddrModeS);

			DBG_OPT_LEA3(new_op1, new_op2, node, new_op);
		} else if (tp1 == ia32_ImmSymConst) {
			tarval *tv = get_ia32_Immop_tarval(new_op2);
			long offs = get_tarval_long(tv);

			new_op = new_rd_ia32_Lea(dbgi, irg, block, noreg, noreg);
			add_irn_dep(new_op, get_irg_frame(irg));
			DBG_OPT_LEA3(new_op1, new_op2, node, new_op);

			set_ia32_am_sc(new_op, get_ia32_Immop_symconst(new_op1));
			add_ia32_am_offs_int(new_op, offs);
			set_ia32_am_flavour(new_op, ia32_am_OB);
			set_ia32_op_type(new_op, ia32_AddrModeS);
		} else if (tp2 == ia32_ImmSymConst) {
			tarval *tv = get_ia32_Immop_tarval(new_op1);
			long offs = get_tarval_long(tv);

			new_op = new_rd_ia32_Lea(dbgi, irg, block, noreg, noreg);
			add_irn_dep(new_op, get_irg_frame(irg));
			DBG_OPT_LEA3(new_op1, new_op2, node, new_op);

			add_ia32_am_offs_int(new_op, offs);
			set_ia32_am_sc(new_op, get_ia32_Immop_symconst(new_op2));
			set_ia32_am_flavour(new_op, ia32_am_OB);
			set_ia32_op_type(new_op, ia32_AddrModeS);
		} else {
			tarval *tv1 = get_ia32_Immop_tarval(new_op1);
			tarval *tv2 = get_ia32_Immop_tarval(new_op2);
			tarval *restv = tarval_add(tv1, tv2);

			DEBUG_ONLY(ir_fprintf(stderr, "Warning: add with 2 consts not folded: %+F\n", node));

			new_op = new_rd_ia32_Const(dbgi, irg, block);
			set_ia32_Const_tarval(new_op, restv);
			DBG_OPT_LEA3(new_op1, new_op2, node, new_op);
		}

		SET_IA32_ORIG_NODE(new_op, ia32_get_old_node_name(env_cg, node));
		return new_op;
	} else if (imm_op) {
		if ((env_cg->opt & IA32_OPT_INCDEC) && get_ia32_immop_type(imm_op) == ia32_ImmConst) {
			tarval_classification_t class_tv, class_negtv;
			tarval *tv = get_ia32_Immop_tarval(imm_op);

			/* optimize tarvals */
			class_tv    = classify_tarval(tv);
			class_negtv = classify_tarval(tarval_neg(tv));

			if (class_tv == TV_CLASSIFY_ONE) { /* + 1 == INC */
				DB((dbg, LEVEL_2, "Add(1) to Inc ... "));
				new_op     = new_rd_ia32_Inc(dbgi, irg, block, noreg, noreg, expr_op, nomem);
				SET_IA32_ORIG_NODE(new_op, ia32_get_old_node_name(env_cg, node));
				return new_op;
			} else if (class_tv == TV_CLASSIFY_ALL_ONE || class_negtv == TV_CLASSIFY_ONE) { /* + (-1) == DEC */
				DB((dbg, LEVEL_2, "Add(-1) to Dec ... "));
				new_op     = new_rd_ia32_Dec(dbgi, irg, block, noreg, noreg, expr_op, nomem);
				SET_IA32_ORIG_NODE(new_op, ia32_get_old_node_name(env_cg, node));
				return new_op;
			}
		}
	}

	/* This is a normal add */
	new_op = new_rd_ia32_Add(dbgi, irg, block, noreg, noreg, new_op1, new_op2, nomem);

	/* set AM support */
	set_ia32_am_support(new_op, ia32_am_Full, ia32_am_binary);
	set_ia32_commutative(new_op);

	fold_immediate(new_op, 2, 3);

	SET_IA32_ORIG_NODE(new_op, ia32_get_old_node_name(env_cg, node));

	return new_op;
}

/**
 * Creates an ia32 Mul.
 *
 * @return the created ia32 Mul node
 */
static ir_node *gen_Mul(ir_node *node) {
	ir_node *op1  = get_Mul_left(node);
	ir_node *op2  = get_Mul_right(node);
	ir_mode *mode = get_irn_mode(node);

	if (mode_is_float(mode)) {
		if (USE_SSE2(env_cg))
			return gen_binop_sse_float(node, op1, op2, new_rd_ia32_xMul);
		else
			return gen_binop_x87_float(node, op1, op2, new_rd_ia32_vfmul);
	}

	/*
		for the lower 32bit of the result it doesn't matter whether we use
		signed or unsigned multiplication so we use IMul as it has fewer
		constraints
	*/
	return gen_binop(node, op1, op2, new_rd_ia32_IMul, 1);
}

/**
 * Creates an ia32 Mulh.
 * Note: Mul produces a 64Bit result and Mulh returns the upper 32 bit of
 * this result while Mul returns the lower 32 bit.
 *
 * @return the created ia32 Mulh node
 */
static ir_node *gen_Mulh(ir_node *node) {
	ir_node  *block   = be_transform_node(get_nodes_block(node));
	ir_node  *op1     = get_irn_n(node, 0);
	ir_node  *new_op1 = be_transform_node(op1);
	ir_node  *op2     = get_irn_n(node, 1);
	ir_node  *new_op2 = be_transform_node(op2);
	ir_graph *irg     = current_ir_graph;
	dbg_info *dbgi    = get_irn_dbg_info(node);
	ir_node  *noreg   = ia32_new_NoReg_gp(env_cg);
	ir_mode  *mode    = get_irn_mode(node);
	ir_node  *proj_EDX, *res;

	assert(!mode_is_float(mode) && "Mulh with float not supported");
	if (mode_is_signed(mode)) {
		res = new_rd_ia32_IMul1OP(dbgi, irg, block, noreg, noreg, new_op1,
		                          new_op2, new_NoMem());
	} else {
		res = new_rd_ia32_Mul(dbgi, irg, block, noreg, noreg, new_op1, new_op2,
		                      new_NoMem());
	}

	set_ia32_commutative(res);
	set_ia32_am_support(res, ia32_am_Source, ia32_am_binary);

	proj_EDX = new_rd_Proj(dbgi, irg, block, res, mode_Iu, pn_EDX);

	return proj_EDX;
}



/**
 * Creates an ia32 And.
 *
 * @return The created ia32 And node
 */
static ir_node *gen_And(ir_node *node) {
	ir_node *op1 = get_And_left(node);
	ir_node *op2 = get_And_right(node);

	assert (! mode_is_float(get_irn_mode(node)));
	return gen_binop(node, op1, op2, new_rd_ia32_And, 1);
}



/**
 * Creates an ia32 Or.
 *
 * @return The created ia32 Or node
 */
static ir_node *gen_Or(ir_node *node) {
	ir_node *op1 = get_Or_left(node);
	ir_node *op2 = get_Or_right(node);

	assert (! mode_is_float(get_irn_mode(node)));
	return gen_binop(node, op1, op2, new_rd_ia32_Or, 1);
}



/**
 * Creates an ia32 Eor.
 *
 * @return The created ia32 Eor node
 */
static ir_node *gen_Eor(ir_node *node) {
	ir_node *op1 = get_Eor_left(node);
	ir_node *op2 = get_Eor_right(node);

	assert(! mode_is_float(get_irn_mode(node)));
	return gen_binop(node, op1, op2, new_rd_ia32_Xor, 1);
}


/**
 * Creates an ia32 Sub.
 *
 * @return The created ia32 Sub node
 */
static ir_node *gen_Sub(ir_node *node) {
	ir_node  *block   = be_transform_node(get_nodes_block(node));
	ir_node  *op1     = get_Sub_left(node);
	ir_node  *new_op1 = be_transform_node(op1);
	ir_node  *op2     = get_Sub_right(node);
	ir_node  *new_op2 = be_transform_node(op2);
	ir_node  *new_op  = NULL;
	ir_graph *irg     = current_ir_graph;
	dbg_info *dbgi    = get_irn_dbg_info(node);
	ir_mode  *mode    = get_irn_mode(node);
	ir_node  *noreg   = ia32_new_NoReg_gp(env_cg);
	ir_node  *nomem   = new_NoMem();
	ir_node  *expr_op, *imm_op;

	/* Check if immediate optimization is on and */
	/* if it's an operation with immediate.      */
	imm_op  = (env_cg->opt & IA32_OPT_IMMOPS) ? get_immediate_op(NULL, new_op2) : NULL;
	expr_op = get_expr_op(new_op1, new_op2);

	assert((expr_op || imm_op) && "invalid operands");

	if (mode_is_float(mode)) {
		if (USE_SSE2(env_cg))
			return gen_binop_sse_float(node, op1, op2, new_rd_ia32_xSub);
		else
			return gen_binop_x87_float(node, op1, op2, new_rd_ia32_vfsub);
	}

	/* integer SUB */
	if (! expr_op) {
		ia32_immop_type_t tp1 = get_ia32_immop_type(new_op1);
		ia32_immop_type_t tp2 = get_ia32_immop_type(new_op2);

		/* No expr_op means, that we have two const - one symconst and */
		/* one tarval or another symconst - because this case is not   */
		/* covered by constant folding                                 */
		/* We need to check for:                                       */
		/*  1) symconst - const    -> becomes a LEA                    */
		/*  2) symconst - symconst -> becomes a const - LEA as the elf */
		/*        linker doesn't support two symconsts                 */
		if (tp1 == ia32_ImmSymConst && tp2 == ia32_ImmSymConst) {
			/* this is the 2nd case */
			new_op = new_rd_ia32_Lea(dbgi, irg, block, new_op1, noreg);
			set_ia32_am_sc(new_op, get_ia32_Immop_symconst(op2));
			set_ia32_am_sc_sign(new_op);
			set_ia32_am_flavour(new_op, ia32_am_B);

			DBG_OPT_LEA3(op1, op2, node, new_op);
		} else if (tp1 == ia32_ImmSymConst) {
			tarval *tv = get_ia32_Immop_tarval(new_op2);
			long offs = get_tarval_long(tv);

			new_op = new_rd_ia32_Lea(dbgi, irg, block, noreg, noreg);
			add_irn_dep(new_op, get_irg_frame(irg));
			DBG_OPT_LEA3(op1, op2, node, new_op);

			set_ia32_am_sc(new_op, get_ia32_Immop_symconst(new_op1));
			add_ia32_am_offs_int(new_op, -offs);
			set_ia32_am_flavour(new_op, ia32_am_OB);
			set_ia32_op_type(new_op, ia32_AddrModeS);
		} else if (tp2 == ia32_ImmSymConst) {
			tarval *tv = get_ia32_Immop_tarval(new_op1);
			long offs = get_tarval_long(tv);

			new_op = new_rd_ia32_Lea(dbgi, irg, block, noreg, noreg);
			add_irn_dep(new_op, get_irg_frame(irg));
			DBG_OPT_LEA3(op1, op2, node, new_op);

			add_ia32_am_offs_int(new_op, offs);
			set_ia32_am_sc(new_op, get_ia32_Immop_symconst(new_op2));
			set_ia32_am_sc_sign(new_op);
			set_ia32_am_flavour(new_op, ia32_am_OB);
			set_ia32_op_type(new_op, ia32_AddrModeS);
		} else {
			tarval *tv1 = get_ia32_Immop_tarval(new_op1);
			tarval *tv2 = get_ia32_Immop_tarval(new_op2);
			tarval *restv = tarval_sub(tv1, tv2);

			DEBUG_ONLY(ir_fprintf(stderr, "Warning: sub with 2 consts not folded: %+F\n", node));

			new_op = new_rd_ia32_Const(dbgi, irg, block);
			set_ia32_Const_tarval(new_op, restv);
			DBG_OPT_LEA3(new_op1, new_op2, node, new_op);
		}

		SET_IA32_ORIG_NODE(new_op, ia32_get_old_node_name(env_cg, node));
		return new_op;
	} else if (imm_op) {
		if ((env_cg->opt & IA32_OPT_INCDEC) && get_ia32_immop_type(imm_op) == ia32_ImmConst) {
			tarval_classification_t class_tv, class_negtv;
			tarval *tv = get_ia32_Immop_tarval(imm_op);

			/* optimize tarvals */
			class_tv    = classify_tarval(tv);
			class_negtv = classify_tarval(tarval_neg(tv));

			if (class_tv == TV_CLASSIFY_ONE) {
				DB((dbg, LEVEL_2, "Sub(1) to Dec ... "));
				new_op     = new_rd_ia32_Dec(dbgi, irg, block, noreg, noreg, expr_op, nomem);
				SET_IA32_ORIG_NODE(new_op, ia32_get_old_node_name(env_cg, node));
				return new_op;
			} else if (class_tv == TV_CLASSIFY_ALL_ONE || class_negtv == TV_CLASSIFY_ONE) {
				DB((dbg, LEVEL_2, "Sub(-1) to Inc ... "));
				new_op     = new_rd_ia32_Inc(dbgi, irg, block, noreg, noreg, expr_op, nomem);
				SET_IA32_ORIG_NODE(new_op, ia32_get_old_node_name(env_cg, node));
				return new_op;
			}
		}
	}

	/* This is a normal sub */
	new_op = new_rd_ia32_Sub(dbgi, irg, block, noreg, noreg, new_op1, new_op2, nomem);

	/* set AM support */
	set_ia32_am_support(new_op, ia32_am_Full, ia32_am_binary);

	fold_immediate(new_op, 2, 3);

	SET_IA32_ORIG_NODE(new_op, ia32_get_old_node_name(env_cg, node));

	return new_op;
}



/**
 * Generates an ia32 DivMod with additional infrastructure for the
 * register allocator if needed.
 *
 * @param dividend -no comment- :)
 * @param divisor  -no comment- :)
 * @param dm_flav  flavour_Div/Mod/DivMod
 * @return The created ia32 DivMod node
 */
static ir_node *generate_DivMod(ir_node *node, ir_node *dividend,
                                ir_node *divisor, ia32_op_flavour_t dm_flav)
{
	ir_node  *block        = be_transform_node(get_nodes_block(node));
	ir_node  *new_dividend = be_transform_node(dividend);
	ir_node  *new_divisor  = be_transform_node(divisor);
	ir_graph *irg          = current_ir_graph;
	dbg_info *dbgi         = get_irn_dbg_info(node);
	ir_mode  *mode         = get_irn_mode(node);
	ir_node  *noreg        = ia32_new_NoReg_gp(env_cg);
	ir_node  *res, *proj_div, *proj_mod;
	ir_node  *sign_extension;
	ir_node  *mem, *new_mem;
	ir_node  *projs[pn_DivMod_max];
	int       has_exc;

	ia32_collect_Projs(node, projs, pn_DivMod_max);

	proj_div = proj_mod = NULL;
	has_exc  = 0;
	switch (dm_flav) {
		case flavour_Div:
			mem  = get_Div_mem(node);
			mode = get_Div_resmode(node);
			proj_div = be_get_Proj_for_pn(node, pn_Div_res);
			has_exc  = be_get_Proj_for_pn(node, pn_Div_X_except) != NULL;
			break;
		case flavour_Mod:
			mem  = get_Mod_mem(node);
			mode = get_Mod_resmode(node);
			proj_mod = be_get_Proj_for_pn(node, pn_Mod_res);
			has_exc  = be_get_Proj_for_pn(node, pn_Mod_X_except) != NULL;
			break;
		case flavour_DivMod:
			mem  = get_DivMod_mem(node);
			mode = get_DivMod_resmode(node);
			proj_div = be_get_Proj_for_pn(node, pn_DivMod_res_div);
			proj_mod = be_get_Proj_for_pn(node, pn_DivMod_res_mod);
			has_exc  = be_get_Proj_for_pn(node, pn_DivMod_X_except) != NULL;
			break;
		default:
			panic("invalid divmod flavour!");
	}
	new_mem = be_transform_node(mem);

	if (mode_is_signed(mode)) {
		/* in signed mode, we need to sign extend the dividend */
		sign_extension = new_rd_ia32_Cltd(dbgi, irg, block, new_dividend);
	} else {
		sign_extension = new_rd_ia32_Const(dbgi, irg, block);
		set_ia32_Immop_tarval(sign_extension, get_tarval_null(mode_Iu));

		add_irn_dep(sign_extension, get_irg_frame(irg));
	}

	if (mode_is_signed(mode)) {
		res = new_rd_ia32_IDiv(dbgi, irg, block, noreg, noreg, new_dividend,
		                       sign_extension, new_divisor, new_mem, dm_flav);
	} else {
		res = new_rd_ia32_Div(dbgi, irg, block, noreg, noreg, new_dividend,
		                      sign_extension, new_divisor, new_mem, dm_flav);
	}

	set_ia32_exc_label(res, has_exc);
	set_irn_pinned(res, get_irn_pinned(node));
	set_ia32_am_support(res, ia32_am_Source, ia32_am_binary);

	SET_IA32_ORIG_NODE(res, ia32_get_old_node_name(env_cg, node));

	return res;
}


/**
 * Wrapper for generate_DivMod. Sets flavour_Mod.
 *
 */
static ir_node *gen_Mod(ir_node *node) {
	return generate_DivMod(node, get_Mod_left(node),
	                       get_Mod_right(node), flavour_Mod);
}

/**
 * Wrapper for generate_DivMod. Sets flavour_Div.
 *
 */
static ir_node *gen_Div(ir_node *node) {
	return generate_DivMod(node, get_Div_left(node),
	                       get_Div_right(node), flavour_Div);
}

/**
 * Wrapper for generate_DivMod. Sets flavour_DivMod.
 */
static ir_node *gen_DivMod(ir_node *node) {
	return generate_DivMod(node, get_DivMod_left(node),
	                       get_DivMod_right(node), flavour_DivMod);
}



/**
 * Creates an ia32 floating Div.
 *
 * @return The created ia32 xDiv node
 */
static ir_node *gen_Quot(ir_node *node) {
	ir_node  *block   = be_transform_node(get_nodes_block(node));
	ir_node  *op1     = get_Quot_left(node);
	ir_node  *new_op1 = be_transform_node(op1);
	ir_node  *op2     = get_Quot_right(node);
	ir_node  *new_op2 = be_transform_node(op2);
	ir_graph *irg     = current_ir_graph;
	dbg_info *dbgi    = get_irn_dbg_info(node);
	ir_node  *noreg   = ia32_new_NoReg_gp(env_cg);
	ir_node  *nomem   = new_rd_NoMem(current_ir_graph);
	ir_node  *new_op;

	if (USE_SSE2(env_cg)) {
		ir_mode *mode = get_irn_mode(op1);
		if (is_ia32_xConst(new_op2)) {
			new_op = new_rd_ia32_xDiv(dbgi, irg, block, noreg, noreg, new_op1, noreg, nomem);
			set_ia32_am_support(new_op, ia32_am_None, ia32_am_arity_none);
			copy_ia32_Immop_attr(new_op, new_op2);
		} else {
			new_op = new_rd_ia32_xDiv(dbgi, irg, block, noreg, noreg, new_op1, new_op2, nomem);
			// Matze: disabled for now, spillslot coalescer fails
			set_ia32_am_support(new_op, ia32_am_Source, ia32_am_binary);
		}
		set_ia32_ls_mode(new_op, mode);
	} else {
		ir_node  *fpcw = be_abi_get_ignore_irn(env_cg->birg->abi,
		                                       &ia32_fp_cw_regs[REG_FPCW]);
		new_op = new_rd_ia32_vfdiv(dbgi, irg, block, noreg, noreg, new_op1,
		                           new_op2, nomem, fpcw);
		// Matze: disabled for now (spillslot coalescer fails)
		set_ia32_am_support(new_op, ia32_am_Source, ia32_am_binary);
	}
	SET_IA32_ORIG_NODE(new_op, ia32_get_old_node_name(env_cg, node));
	return new_op;
}


/**
 * Creates an ia32 Shl.
 *
 * @return The created ia32 Shl node
 */
static ir_node *gen_Shl(ir_node *node) {
	return gen_shift_binop(node, get_Shl_left(node), get_Shl_right(node),
	                       new_rd_ia32_Shl);
}



/**
 * Creates an ia32 Shr.
 *
 * @return The created ia32 Shr node
 */
static ir_node *gen_Shr(ir_node *node) {
	return gen_shift_binop(node, get_Shr_left(node),
	                       get_Shr_right(node), new_rd_ia32_Shr);
}



/**
 * Creates an ia32 Sar.
 *
 * @return The created ia32 Shrs node
 */
static ir_node *gen_Shrs(ir_node *node) {
	ir_node *left  = get_Shrs_left(node);
	ir_node *right = get_Shrs_right(node);
	if(is_Const(right) && get_irn_mode(left) == mode_Is) {
		tarval *tv = get_Const_tarval(right);
		long val = get_tarval_long(tv);
		if(val == 31) {
			/* this is a sign extension */
			ir_graph *irg    = current_ir_graph;
			dbg_info *dbgi   = get_irn_dbg_info(node);
			ir_node  *block  = be_transform_node(get_nodes_block(node));
			ir_node  *op     = left;
			ir_node  *new_op = be_transform_node(op);

			return new_rd_ia32_Cltd(dbgi, irg, block, new_op);
		}
	}

	return gen_shift_binop(node, left, right, new_rd_ia32_Sar);
}



/**
 * Creates an ia32 RotL.
 *
 * @param op1   The first operator
 * @param op2   The second operator
 * @return The created ia32 RotL node
 */
static ir_node *gen_RotL(ir_node *node,
                         ir_node *op1, ir_node *op2) {
	return gen_shift_binop(node, op1, op2, new_rd_ia32_Rol);
}



/**
 * Creates an ia32 RotR.
 * NOTE: There is no RotR with immediate because this would always be a RotL
 *       "imm-mode_size_bits" which can be pre-calculated.
 *
 * @param op1   The first operator
 * @param op2   The second operator
 * @return The created ia32 RotR node
 */
static ir_node *gen_RotR(ir_node *node, ir_node *op1,
                         ir_node *op2) {
	return gen_shift_binop(node, op1, op2, new_rd_ia32_Ror);
}



/**
 * Creates an ia32 RotR or RotL (depending on the found pattern).
 *
 * @return The created ia32 RotL or RotR node
 */
static ir_node *gen_Rot(ir_node *node) {
	ir_node *rotate = NULL;
	ir_node *op1    = get_Rot_left(node);
	ir_node *op2    = get_Rot_right(node);

	/* Firm has only Rot (which is a RotL), so we are looking for a right (op2)
		 operand "-e+mode_size_bits" (it's an already modified "mode_size_bits-e",
		 that means we can create a RotR instead of an Add and a RotL */

	if (get_irn_op(op2) == op_Add) {
		ir_node *add = op2;
		ir_node *left = get_Add_left(add);
		ir_node *right = get_Add_right(add);
		if (is_Const(right)) {
			tarval  *tv   = get_Const_tarval(right);
			ir_mode *mode = get_irn_mode(node);
			long     bits = get_mode_size_bits(mode);

			if (get_irn_op(left) == op_Minus &&
					tarval_is_long(tv)       &&
					get_tarval_long(tv) == bits)
			{
				DB((dbg, LEVEL_1, "RotL into RotR ... "));
				rotate = gen_RotR(node, op1, get_Minus_op(left));
			}
		}
	}

	if (rotate == NULL) {
		rotate = gen_RotL(node, op1, op2);
	}

	return rotate;
}



/**
 * Transforms a Minus node.
 *
 * @param op    The Minus operand
 * @return The created ia32 Minus node
 */
ir_node *gen_Minus_ex(ir_node *node, ir_node *op) {
	ir_node   *block = be_transform_node(get_nodes_block(node));
	ir_graph  *irg   = current_ir_graph;
	dbg_info  *dbgi  = get_irn_dbg_info(node);
	ir_mode   *mode  = get_irn_mode(node);
	ir_entity *ent;
	ir_node   *res;
	int       size;

	if (mode_is_float(mode)) {
		ir_node *new_op = be_transform_node(op);
		if (USE_SSE2(env_cg)) {
			ir_node *noreg_gp = ia32_new_NoReg_gp(env_cg);
			ir_node *noreg_fp = ia32_new_NoReg_fp(env_cg);
			ir_node *nomem    = new_rd_NoMem(irg);

			res = new_rd_ia32_xXor(dbgi, irg, block, noreg_gp, noreg_gp, new_op, noreg_fp, nomem);

			size = get_mode_size_bits(mode);
			ent  = ia32_gen_fp_known_const(size == 32 ? ia32_SSIGN : ia32_DSIGN);

			set_ia32_am_sc(res, ent);
			set_ia32_op_type(res, ia32_AddrModeS);
			set_ia32_ls_mode(res, mode);
		} else {
			res = new_rd_ia32_vfchs(dbgi, irg, block, new_op);
		}
	} else {
		res = gen_unop(node, op, new_rd_ia32_Neg);
	}

	SET_IA32_ORIG_NODE(res, ia32_get_old_node_name(env_cg, node));

	return res;
}

/**
 * Transforms a Minus node.
 *
 * @return The created ia32 Minus node
 */
static ir_node *gen_Minus(ir_node *node) {
	return gen_Minus_ex(node, get_Minus_op(node));
}

static ir_node *gen_bin_Not(ir_node *node)
{
	ir_graph *irg    = current_ir_graph;
	dbg_info *dbgi   = get_irn_dbg_info(node);
	ir_node  *block  = be_transform_node(get_nodes_block(node));
	ir_node  *op     = get_Not_op(node);
	ir_node  *new_op = be_transform_node(op);
	ir_node  *noreg  = ia32_new_NoReg_gp(env_cg);
	ir_node  *nomem  = new_NoMem();
	ir_node  *one    = new_rd_ia32_Immediate(dbgi, irg, block, NULL, 0, 1);
	arch_set_irn_register(env_cg->arch_env, one, &ia32_gp_regs[REG_GP_NOREG]);

	return new_rd_ia32_Xor(dbgi, irg, block, noreg, noreg, new_op, one, nomem);
}

/**
 * Transforms a Not node.
 *
 * @return The created ia32 Not node
 */
static ir_node *gen_Not(ir_node *node) {
	ir_node *op   = get_Not_op(node);
	ir_mode *mode = get_irn_mode(node);

	if(mode == mode_b) {
		return gen_bin_Not(node);
	}

	assert (! mode_is_float(get_irn_mode(node)));
	return gen_unop(node, op, new_rd_ia32_Not);
}



/**
 * Transforms an Abs node.
 *
 * @return The created ia32 Abs node
 */
static ir_node *gen_Abs(ir_node *node) {
	ir_node   *block    = be_transform_node(get_nodes_block(node));
	ir_node   *op       = get_Abs_op(node);
	ir_node   *new_op   = be_transform_node(op);
	ir_graph  *irg      = current_ir_graph;
	dbg_info  *dbgi     = get_irn_dbg_info(node);
	ir_mode   *mode     = get_irn_mode(node);
	ir_node   *noreg_gp = ia32_new_NoReg_gp(env_cg);
	ir_node   *noreg_fp = ia32_new_NoReg_fp(env_cg);
	ir_node   *nomem    = new_NoMem();
	ir_node   *res;
	int       size;
	ir_entity *ent;

	if (mode_is_float(mode)) {
		if (USE_SSE2(env_cg)) {
			res = new_rd_ia32_xAnd(dbgi,irg, block, noreg_gp, noreg_gp, new_op, noreg_fp, nomem);

			size = get_mode_size_bits(mode);
			ent  = ia32_gen_fp_known_const(size == 32 ? ia32_SABS : ia32_DABS);

			set_ia32_am_sc(res, ent);

			SET_IA32_ORIG_NODE(res, ia32_get_old_node_name(env_cg, node));

			set_ia32_op_type(res, ia32_AddrModeS);
			set_ia32_ls_mode(res, mode);
		}
		else {
			res = new_rd_ia32_vfabs(dbgi, irg, block, new_op);
			SET_IA32_ORIG_NODE(res, ia32_get_old_node_name(env_cg, node));
		}
	} else {
		ir_node *xor;
		ir_node *sign_extension = new_rd_ia32_Cltd(dbgi, irg, block, new_op);
		SET_IA32_ORIG_NODE(sign_extension,
		                   ia32_get_old_node_name(env_cg, node));

		xor = new_rd_ia32_Xor(dbgi, irg, block, noreg_gp, noreg_gp, new_op,
		                      sign_extension, nomem);
		SET_IA32_ORIG_NODE(xor, ia32_get_old_node_name(env_cg, node));

		res = new_rd_ia32_Sub(dbgi, irg, block, noreg_gp, noreg_gp, xor,
		                      sign_extension, nomem);
		SET_IA32_ORIG_NODE(res, ia32_get_old_node_name(env_cg, node));
	}

	return res;
}



/**
 * Transforms a Load.
 *
 * @return the created ia32 Load node
 */
static ir_node *gen_Load(ir_node *node) {
	ir_node *old_block = get_nodes_block(node);
	ir_node  *block   = be_transform_node(old_block);
	ir_node  *ptr     = get_Load_ptr(node);
	ir_node  *new_ptr = be_transform_node(ptr);
	ir_node  *mem     = get_Load_mem(node);
	ir_node  *new_mem = be_transform_node(mem);
	ir_graph *irg     = current_ir_graph;
	dbg_info *dbgi    = get_irn_dbg_info(node);
	ir_node  *noreg   = ia32_new_NoReg_gp(env_cg);
	ir_mode  *mode    = get_Load_mode(node);
	ir_mode  *res_mode;
	ir_node  *lptr    = new_ptr;
	int      is_imm   = 0;
	ir_node  *new_op;
	ia32_am_flavour_t am_flav = ia32_am_B;

	/* address might be a constant (symconst or absolute address) */
	if (is_ia32_Const(new_ptr)) {
		lptr   = noreg;
		is_imm = 1;
	}

	if (mode_is_float(mode)) {
		if (USE_SSE2(env_cg)) {
			new_op  = new_rd_ia32_xLoad(dbgi, irg, block, lptr, noreg, new_mem);
			res_mode = mode_xmm;
		} else {
			new_op   = new_rd_ia32_vfld(dbgi, irg, block, lptr, noreg, new_mem, mode);
			res_mode = mode_vfp;
		}
	} else {
		new_op   = new_rd_ia32_Load(dbgi, irg, block, lptr, noreg, new_mem);
		res_mode = mode_Iu;
	}

	/* base is a constant address */
	if (is_imm) {
		if (get_ia32_immop_type(new_ptr) == ia32_ImmSymConst) {
			set_ia32_am_sc(new_op, get_ia32_Immop_symconst(new_ptr));
			am_flav = ia32_am_N;
		} else {
			tarval *tv = get_ia32_Immop_tarval(new_ptr);
			long offs = get_tarval_long(tv);

			add_ia32_am_offs_int(new_op, offs);
			am_flav = ia32_am_O;
		}
	}

	set_irn_pinned(new_op, get_irn_pinned(node));
	set_ia32_op_type(new_op, ia32_AddrModeS);
	set_ia32_am_flavour(new_op, am_flav);
	set_ia32_ls_mode(new_op, mode);

	/* make sure we are scheduled behind the initial IncSP/Barrier
	 * to avoid spills being placed before it
	 */
	if (block == get_irg_start_block(irg)) {
		add_irn_dep(new_op, get_irg_frame(irg));
	}

	set_ia32_exc_label(new_op, be_get_Proj_for_pn(node, pn_Load_X_except) != NULL);
	SET_IA32_ORIG_NODE(new_op, ia32_get_old_node_name(env_cg, node));

	return new_op;
}



/**
 * Transforms a Store.
 *
 * @return the created ia32 Store node
 */
static ir_node *gen_Store(ir_node *node) {
	ir_node  *block   = be_transform_node(get_nodes_block(node));
	ir_node  *ptr     = get_Store_ptr(node);
	ir_node  *new_ptr = be_transform_node(ptr);
	ir_node  *val     = get_Store_value(node);
	ir_node  *new_val;
	ir_node  *mem     = get_Store_mem(node);
	ir_node  *new_mem = be_transform_node(mem);
	ir_graph *irg     = current_ir_graph;
	dbg_info *dbgi    = get_irn_dbg_info(node);
	ir_node  *noreg   = ia32_new_NoReg_gp(env_cg);
	ir_node  *sptr    = new_ptr;
	ir_mode  *mode    = get_irn_mode(val);
	int      is_imm   = 0;
	ir_node  *new_op;
	ia32_am_flavour_t am_flav = ia32_am_B;

	/* address might be a constant (symconst or absolute address) */
	if (is_ia32_Const(new_ptr)) {
		sptr   = noreg;
		is_imm = 1;
	}

	if (mode_is_float(mode)) {
		new_val = be_transform_node(val);
		if (USE_SSE2(env_cg)) {
			new_op = new_rd_ia32_xStore(dbgi, irg, block, sptr, noreg, new_val,
			                            new_mem);
		} else {
			new_op = new_rd_ia32_vfst(dbgi, irg, block, sptr, noreg, new_val,
			                          new_mem, mode);
		}
	} else {
		new_val = create_immediate_or_transform(val, 0);

		if (get_mode_size_bits(mode) == 8) {
			new_op = new_rd_ia32_Store8Bit(dbgi, irg, block, sptr, noreg,
			                               new_val, new_mem);
		} else {
			new_op = new_rd_ia32_Store(dbgi, irg, block, sptr, noreg, new_val,
			                           new_mem);
		}
	}

	/* base is an constant address */
	if (is_imm) {
		if (get_ia32_immop_type(new_ptr) == ia32_ImmSymConst) {
			set_ia32_am_sc(new_op, get_ia32_Immop_symconst(new_ptr));
			am_flav = ia32_am_N;
		} else {
			tarval *tv = get_ia32_Immop_tarval(new_ptr);
			long offs = get_tarval_long(tv);

			add_ia32_am_offs_int(new_op, offs);
			am_flav = ia32_am_O;
		}
	}

	set_irn_pinned(new_op, get_irn_pinned(node));
	set_ia32_op_type(new_op, ia32_AddrModeD);
	set_ia32_am_flavour(new_op, am_flav);
	set_ia32_ls_mode(new_op, mode);

	set_ia32_exc_label(new_op, be_get_Proj_for_pn(node, pn_Store_X_except) != NULL);
	SET_IA32_ORIG_NODE(new_op, ia32_get_old_node_name(env_cg, node));

	return new_op;
}

static ir_node *try_create_TestJmp(ir_node *block, dbg_info *dbgi, long pnc,
                                   ir_node *cmp_left, ir_node *cmp_right)
{
	ir_node  *new_cmp_left;
	ir_node  *new_cmp_right;
	ir_node  *and_left;
	ir_node  *and_right;
	ir_node  *res;
	ir_node  *noreg;
	ir_node  *nomem;
	long      pure_pnc = pnc & ~ia32_pn_Cmp_Unsigned;

	if(cmp_right != NULL && !is_Const_0(cmp_right))
		return NULL;

	if(is_And(cmp_left) && (pure_pnc == pn_Cmp_Eq || pure_pnc == pn_Cmp_Lg)) {
		and_left  = get_And_left(cmp_left);
		and_right = get_And_right(cmp_left);

		new_cmp_left  = be_transform_node(and_left);
		new_cmp_right = create_immediate_or_transform(and_right, 0);
	} else {
		new_cmp_left  = be_transform_node(cmp_left);
		new_cmp_right = be_transform_node(cmp_left);
	}

	noreg     = ia32_new_NoReg_gp(env_cg);
	nomem     = new_NoMem();

	res = new_rd_ia32_TestJmp(dbgi, current_ir_graph, block, noreg, noreg,
	                          new_cmp_left, new_cmp_right, nomem, pnc);
	set_ia32_am_support(res, ia32_am_Source, ia32_am_binary);

	return res;
}

static ir_node *create_Switch(ir_node *node)
{
	ir_graph *irg     = current_ir_graph;
	dbg_info *dbgi    = get_irn_dbg_info(node);
	ir_node  *block   = be_transform_node(get_nodes_block(node));
	ir_node  *sel     = get_Cond_selector(node);
	ir_node  *new_sel = be_transform_node(sel);
	ir_node  *res;
	int switch_min    = INT_MAX;
	const ir_edge_t *edge;

	/* determine the smallest switch case value */
	foreach_out_edge(node, edge) {
		ir_node *proj = get_edge_src_irn(edge);
		int      pn   = get_Proj_proj(proj);
		if(pn < switch_min)
			switch_min = pn;
	}

	if (switch_min != 0) {
		ir_node  *noreg    = ia32_new_NoReg_gp(env_cg);

		/* if smallest switch case is not 0 we need an additional sub */
		new_sel = new_rd_ia32_Lea(dbgi, irg, block, new_sel, noreg);
		add_ia32_am_offs_int(new_sel, -switch_min);
		set_ia32_am_flavour(new_sel, ia32_am_OB);
		set_ia32_op_type(new_sel, ia32_AddrModeS);

		SET_IA32_ORIG_NODE(new_sel, ia32_get_old_node_name(env_cg, node));
	}

	res = new_rd_ia32_SwitchJmp(dbgi, irg, block, new_sel);
	set_ia32_pncode(res, get_Cond_defaultProj(node));

	SET_IA32_ORIG_NODE(res, ia32_get_old_node_name(env_cg, node));

	return res;
}

/**
 * Transforms a Cond -> Proj[b] -> Cmp into a CondJmp, CondJmp_i or TestJmp
 *
 * @return The transformed node.
 */
static ir_node *gen_Cond(ir_node *node) {
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_graph *irg      = current_ir_graph;
	dbg_info *dbgi     = get_irn_dbg_info(node);
	ir_node	 *sel      = get_Cond_selector(node);
	ir_mode  *sel_mode = get_irn_mode(sel);
	ir_node  *res      = NULL;
	ir_node  *noreg    = ia32_new_NoReg_gp(env_cg);
	ir_node  *cmp;
	ir_node  *cmp_a;
	ir_node  *cmp_b;
	ir_node  *new_cmp_a;
	ir_node  *new_cmp_b;
	ir_mode  *cmp_mode;
	ir_node  *nomem = new_NoMem();
	long      pnc;

	if (sel_mode != mode_b) {
		return create_Switch(node);
	}

	if(!is_Proj(sel) || !is_Cmp(get_Proj_pred(sel))) {
		/* it's some mode_b value not a direct comparison -> create a testjmp */
		res = try_create_TestJmp(block, dbgi, pn_Cmp_Lg, sel, NULL);
		SET_IA32_ORIG_NODE(res, ia32_get_old_node_name(env_cg, node));
		return res;
	}

	cmp      = get_Proj_pred(sel);
	cmp_a    = get_Cmp_left(cmp);
	cmp_b    = get_Cmp_right(cmp);
	cmp_mode = get_irn_mode(cmp_a);
	pnc = get_Proj_proj(sel);
	if(mode_is_float(cmp_mode) || !mode_is_signed(cmp_mode)) {
		pnc |= ia32_pn_Cmp_Unsigned;
	}

	if(mode_needs_gp_reg(cmp_mode)) {
		res = try_create_TestJmp(block, dbgi, pnc, cmp_a, cmp_b);
		if(res != NULL) {
			SET_IA32_ORIG_NODE(res, ia32_get_old_node_name(env_cg, node));
			return res;
		}
	}

	new_cmp_a = be_transform_node(cmp_a);
	new_cmp_b = create_immediate_or_transform(cmp_b, 0);

	if (mode_is_float(cmp_mode)) {
		if (USE_SSE2(env_cg)) {
			res = new_rd_ia32_xCondJmp(dbgi, irg, block, noreg, noreg, cmp_a,
			                           cmp_b, nomem, pnc);
			set_ia32_commutative(res);
			set_ia32_am_support(res, ia32_am_Source, ia32_am_binary);
			set_ia32_ls_mode(res, cmp_mode);
		} else {
			res = new_rd_ia32_vfCondJmp(dbgi, irg, block, cmp_a, cmp_b, pnc);
			set_ia32_commutative(res);
		}
	} else {
		assert(get_mode_size_bits(cmp_mode) == 32);
		res = new_rd_ia32_CondJmp(dbgi, irg, block, noreg, noreg,
		                          new_cmp_a, new_cmp_b, nomem, pnc);
		set_ia32_commutative(res);
		set_ia32_am_support(res, ia32_am_Source, ia32_am_binary);
	}

	SET_IA32_ORIG_NODE(res, ia32_get_old_node_name(env_cg, node));

	return res;
}



/**
 * Transforms a CopyB node.
 *
 * @return The transformed node.
 */
static ir_node *gen_CopyB(ir_node *node) {
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_node  *src      = get_CopyB_src(node);
	ir_node  *new_src  = be_transform_node(src);
	ir_node  *dst      = get_CopyB_dst(node);
	ir_node  *new_dst  = be_transform_node(dst);
	ir_node  *mem      = get_CopyB_mem(node);
	ir_node  *new_mem  = be_transform_node(mem);
	ir_node  *res      = NULL;
	ir_graph *irg      = current_ir_graph;
	dbg_info *dbgi     = get_irn_dbg_info(node);
	int      size      = get_type_size_bytes(get_CopyB_type(node));
	int      rem;

	/* If we have to copy more than 32 bytes, we use REP MOVSx and */
	/* then we need the size explicitly in ECX.                    */
	if (size >= 32 * 4) {
		rem = size & 0x3; /* size % 4 */
		size >>= 2;

		res = new_rd_ia32_Const(dbgi, irg, block);
		add_irn_dep(res, be_abi_get_start_barrier(env_cg->birg->abi));
		set_ia32_Immop_tarval(res, new_tarval_from_long(size, mode_Is));

		res = new_rd_ia32_CopyB(dbgi, irg, block, new_dst, new_src, res, new_mem);
		set_ia32_Immop_tarval(res, new_tarval_from_long(rem, mode_Is));
	} else {
		res = new_rd_ia32_CopyB_i(dbgi, irg, block, new_dst, new_src, new_mem);
		set_ia32_Immop_tarval(res, new_tarval_from_long(size, mode_Is));
	}

	SET_IA32_ORIG_NODE(res, ia32_get_old_node_name(env_cg, node));

	return res;
}

static
ir_node *gen_be_Copy(ir_node *node)
{
	ir_node *result = be_duplicate_node(node);
	ir_mode *mode   = get_irn_mode(result);

	if (mode_needs_gp_reg(mode)) {
		set_irn_mode(result, mode_Iu);
	}

	return result;
}


static ir_node *create_set(long pnc, ir_node *cmp_left, ir_node *cmp_right,
                           dbg_info *dbgi, ir_node *block)
{
	ir_graph *irg   = current_ir_graph;
	ir_node  *noreg = ia32_new_NoReg_gp(env_cg);
	ir_node  *nomem = new_rd_NoMem(irg);
	ir_node  *new_cmp_left;
	ir_node  *new_cmp_right;
	ir_node  *res;

	/* can we use a test instruction? */
	if(cmp_right == NULL || is_Const_0(cmp_right)) {
		long pure_pnc = pnc & ~ia32_pn_Cmp_Unsigned;
		if(is_And(cmp_left) &&
				(pure_pnc == pn_Cmp_Eq || pure_pnc == pn_Cmp_Lg)) {
			ir_node *and_left  = get_And_left(cmp_left);
			ir_node *and_right = get_And_right(cmp_left);

			new_cmp_left  = be_transform_node(and_left);
			new_cmp_right = create_immediate_or_transform(and_right, 0);
		} else {
			new_cmp_left  = be_transform_node(cmp_left);
			new_cmp_right = be_transform_node(cmp_left);
		}

		res = new_rd_ia32_TestSet(dbgi, current_ir_graph, block, noreg, noreg,
		                          new_cmp_left, new_cmp_right, nomem, pnc);
		set_ia32_am_support(res, ia32_am_Source, ia32_am_binary);

		return res;
	}

	new_cmp_left  = be_transform_node(cmp_left);
	new_cmp_right = create_immediate_or_transform(cmp_right, 0);
	res           = new_rd_ia32_CmpSet(dbgi, irg, block, noreg, noreg,
	                                   new_cmp_left, new_cmp_right, nomem, pnc);

	return res;
}

static ir_node *create_cmov(long pnc, ir_node *cmp_left, ir_node *cmp_right,
                            ir_node *val_true, ir_node *val_false,
							dbg_info *dbgi, ir_node *block)
{
	ir_graph *irg           = current_ir_graph;
	ir_node  *new_val_true  = be_transform_node(val_true);
	ir_node  *new_val_false = be_transform_node(val_false);
	ir_node  *noreg         = ia32_new_NoReg_gp(env_cg);
	ir_node  *nomem         = new_NoMem();
	ir_node  *new_cmp_left;
	ir_node  *new_cmp_right;
	ir_node  *res;

	/* cmovs with unknowns are pointless... */
	if(is_Unknown(val_true)) {
#ifdef DEBUG_libfirm
		ir_fprintf(stderr, "Optimisation warning: psi with unknown operand\n");
#endif
		return new_val_false;
	}
	if(is_Unknown(val_false)) {
#ifdef DEBUG_libfirm
		ir_fprintf(stderr, "Optimisation warning: psi with unknown operand\n");
#endif
		return new_val_true;
	}

	/* can we use a test instruction? */
	if(is_Const_0(cmp_right)) {
		long pure_pnc = pnc & ~ia32_pn_Cmp_Unsigned;
		if(is_And(cmp_left) &&
				(pure_pnc == pn_Cmp_Eq || pure_pnc == pn_Cmp_Lg)) {
			ir_node *and_left  = get_And_left(cmp_left);
			ir_node *and_right = get_And_right(cmp_left);

			new_cmp_left  = be_transform_node(and_left);
			new_cmp_right = create_immediate_or_transform(and_right, 0);
		} else {
			new_cmp_left  = be_transform_node(cmp_left);
			new_cmp_right = be_transform_node(cmp_left);
		}

		res = new_rd_ia32_TestCMov(dbgi, current_ir_graph, block, noreg, noreg,
		                           new_cmp_left, new_cmp_right, nomem,
		                           new_val_true, new_val_false, pnc);
		set_ia32_am_support(res, ia32_am_Source, ia32_am_binary);

		return res;
	}

	new_cmp_left  = be_transform_node(cmp_left);
	new_cmp_right = create_immediate_or_transform(cmp_right, 0);

	res = new_rd_ia32_CmpCMov(dbgi, irg, block, noreg, noreg, new_cmp_left,
	                          new_cmp_right, nomem, new_val_true, new_val_false,
	                          pnc);
	set_ia32_am_support(res, ia32_am_Source, ia32_am_binary);

	return res;
}


/**
 * Transforms a Psi node into CMov.
 *
 * @return The transformed node.
 */
static ir_node *gen_Psi(ir_node *node) {
	ir_node  *psi_true    = get_Psi_val(node, 0);
	ir_node  *psi_default = get_Psi_default(node);
	ia32_code_gen_t *cg   = env_cg;
	ir_node  *cond        = get_Psi_cond(node, 0);
	ir_node  *block       = be_transform_node(get_nodes_block(node));
	dbg_info *dbgi        = get_irn_dbg_info(node);
	ir_node  *new_op;
	ir_node  *cmp_left;
	ir_node  *cmp_right;
	ir_mode  *cmp_mode;
	long      pnc;

	assert(get_Psi_n_conds(node) == 1);
	assert(get_irn_mode(cond) == mode_b);

	if(!is_Proj(cond) || !is_Cmp(get_Proj_pred(cond))) {
		/* a mode_b value, we have to compare it against 0 */
		cmp_left  = cond;
		cmp_right = new_Const_long(mode_Iu, 0);
		pnc       = pn_Cmp_Lg;
		cmp_mode  = mode_Iu;
	} else {
		ir_node *cmp = get_Proj_pred(cond);

		cmp_left  = get_Cmp_left(cmp);
		cmp_right = get_Cmp_right(cmp);
		cmp_mode  = get_irn_mode(cmp_left);
		pnc       = get_Proj_proj(cond);

		assert(!mode_is_float(cmp_mode));

		if (!mode_is_signed(cmp_mode)) {
			pnc |= ia32_pn_Cmp_Unsigned;
		}
	}

	if(is_Const_1(psi_true) && is_Const_0(psi_default)) {
		new_op = create_set(pnc, cmp_left, cmp_right, dbgi, block);
	} else if(is_Const_0(psi_true) && is_Const_1(psi_default)) {
		pnc = get_negated_pnc(pnc, cmp_mode);
		new_op = create_set(pnc, cmp_left, cmp_right, dbgi, block);
	} else {
		new_op = create_cmov(pnc, cmp_left, cmp_right, psi_true, psi_default,
		                     dbgi, block);
	}
	SET_IA32_ORIG_NODE(new_op, ia32_get_old_node_name(cg, node));
	return new_op;
}


/**
 * Following conversion rules apply:
 *
 *  INT -> INT
 * ============
 *  1) n bit -> m bit   n > m (downscale)
 *     always ignored
 *  2) n bit -> m bit   n == m   (sign change)
 *     always ignored
 *  3) n bit -> m bit   n < m (upscale)
 *     a) source is signed:    movsx
 *     b) source is unsigned:  and with lower bits sets
 *
 *  INT -> FLOAT
 * ==============
 *  SSE(1/2) convert to float or double (cvtsi2ss/sd)
 *
 *  FLOAT -> INT
 * ==============
 *  SSE(1/2) convert from float or double to 32bit int (cvtss/sd2si)
 *
 *  FLOAT -> FLOAT
 * ================
 *  SSE(1/2) convert from float or double to double or float (cvtss/sd2sd/ss)
 *  x87 is mode_E internally, conversions happen only at load and store
 *  in non-strict semantic
 */

/**
 * Create a conversion from x87 state register to general purpose.
 */
static ir_node *gen_x87_fp_to_gp(ir_node *node) {
	ir_node         *block      = be_transform_node(get_nodes_block(node));
	ir_node         *op         = get_Conv_op(node);
	ir_node         *new_op     = be_transform_node(op);
	ia32_code_gen_t *cg         = env_cg;
	ir_graph        *irg        = current_ir_graph;
	dbg_info        *dbgi       = get_irn_dbg_info(node);
	ir_node         *noreg      = ia32_new_NoReg_gp(cg);
	ir_node         *trunc_mode = ia32_new_Fpu_truncate(cg);
	ir_node         *fist, *load;

	/* do a fist */
	fist = new_rd_ia32_vfist(dbgi, irg, block,
			get_irg_frame(irg), noreg, new_op, trunc_mode, new_NoMem());

	set_irn_pinned(fist, op_pin_state_floats);
	set_ia32_use_frame(fist);
	set_ia32_op_type(fist, ia32_AddrModeD);
	set_ia32_am_flavour(fist, ia32_am_B);
	set_ia32_ls_mode(fist, mode_Iu);
	SET_IA32_ORIG_NODE(fist, ia32_get_old_node_name(cg, node));

	/* do a Load */
	load = new_rd_ia32_Load(dbgi, irg, block, get_irg_frame(irg), noreg, fist);

	set_irn_pinned(load, op_pin_state_floats);
	set_ia32_use_frame(load);
	set_ia32_op_type(load, ia32_AddrModeS);
	set_ia32_am_flavour(load, ia32_am_B);
	set_ia32_ls_mode(load, mode_Iu);
	SET_IA32_ORIG_NODE(load, ia32_get_old_node_name(cg, node));

	return new_r_Proj(irg, block, load, mode_Iu, pn_ia32_Load_res);
}

static ir_node *create_strict_conv(ir_mode *src_mode, ir_mode *tgt_mode,
                                   ir_node *node)
{
	ir_node  *block    = get_nodes_block(node);
	ir_graph *irg      = current_ir_graph;
	dbg_info *dbgi     = get_irn_dbg_info(node);
	ir_node  *noreg    = ia32_new_NoReg_gp(env_cg);
	ir_node  *nomem    = new_NoMem();
	int       src_bits = get_mode_size_bits(src_mode);
	int       tgt_bits = get_mode_size_bits(tgt_mode);
	ir_node  *frame    = get_irg_frame(irg);
	ir_mode  *smaller_mode;
	ir_node  *store, *load;
	ir_node  *res;

	if(src_bits <= tgt_bits)
		smaller_mode = src_mode;
	else
		smaller_mode = tgt_mode;

	store = new_rd_ia32_vfst(dbgi, irg, block, frame, noreg, node, nomem,
	                         smaller_mode);
	set_ia32_use_frame(store);
	set_ia32_op_type(store, ia32_AddrModeD);
	set_ia32_am_flavour(store, ia32_am_OB);
	SET_IA32_ORIG_NODE(store, ia32_get_old_node_name(env_cg, node));

	load = new_rd_ia32_vfld(dbgi, irg, block, frame, noreg, store,
	                        smaller_mode);
	set_ia32_use_frame(load);
	set_ia32_op_type(load, ia32_AddrModeS);
	set_ia32_am_flavour(load, ia32_am_OB);
	SET_IA32_ORIG_NODE(load, ia32_get_old_node_name(env_cg, node));

	res = new_r_Proj(irg, block, load, mode_E, pn_ia32_vfld_res);
	return res;
}

/**
 * Create a conversion from general purpose to x87 register
 */
static ir_node *gen_x87_gp_to_fp(ir_node *node, ir_mode *src_mode) {
	ir_node   *block  = be_transform_node(get_nodes_block(node));
	ir_node   *op     = get_Conv_op(node);
	ir_node   *new_op = be_transform_node(op);
	ir_graph  *irg    = current_ir_graph;
	dbg_info  *dbgi   = get_irn_dbg_info(node);
	ir_node   *noreg  = ia32_new_NoReg_gp(env_cg);
	ir_node   *nomem  = new_NoMem();
	ir_node   *fild, *store;
	ir_node   *res;
	int        src_bits;

	/* first convert to 32 bit if necessary */
	src_bits = get_mode_size_bits(src_mode);
	if (src_bits == 8) {
		new_op = new_rd_ia32_Conv_I2I8Bit(dbgi, irg, block, noreg, noreg, new_op, nomem);
		set_ia32_am_support(new_op, ia32_am_Source, ia32_am_unary);
		set_ia32_ls_mode(new_op, src_mode);
		SET_IA32_ORIG_NODE(new_op, ia32_get_old_node_name(env_cg, node));
	} else if (src_bits < 32) {
		new_op = new_rd_ia32_Conv_I2I(dbgi, irg, block, noreg, noreg, new_op, nomem);
		set_ia32_am_support(new_op, ia32_am_Source, ia32_am_unary);
		set_ia32_ls_mode(new_op, src_mode);
		SET_IA32_ORIG_NODE(new_op, ia32_get_old_node_name(env_cg, node));
	}

	/* do a store */
	store = new_rd_ia32_Store(dbgi, irg, block, get_irg_frame(irg), noreg, new_op, nomem);

	set_ia32_use_frame(store);
	set_ia32_op_type(store, ia32_AddrModeD);
	set_ia32_am_flavour(store, ia32_am_OB);
	set_ia32_ls_mode(store, mode_Iu);

	/* do a fild */
	fild = new_rd_ia32_vfild(dbgi, irg, block, get_irg_frame(irg), noreg, store);

	set_ia32_use_frame(fild);
	set_ia32_op_type(fild, ia32_AddrModeS);
	set_ia32_am_flavour(fild, ia32_am_OB);
	set_ia32_ls_mode(fild, mode_Iu);

	res = new_r_Proj(irg, block, fild, mode_vfp, pn_ia32_vfild_res);

	if(get_irg_fp_model(irg) & fp_explicit_rounding) {
		res = create_strict_conv(mode_E, get_irn_mode(node), res);
	}

	return res;
}

/**
 * Transforms a Conv node.
 *
 * @return The created ia32 Conv node
 */
static ir_node *gen_Conv(ir_node *node) {
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_node  *op       = get_Conv_op(node);
	ir_node  *new_op   = be_transform_node(op);
	ir_graph *irg      = current_ir_graph;
	dbg_info *dbgi     = get_irn_dbg_info(node);
	ir_mode  *src_mode = get_irn_mode(op);
	ir_mode  *tgt_mode = get_irn_mode(node);
	int       src_bits = get_mode_size_bits(src_mode);
	int       tgt_bits = get_mode_size_bits(tgt_mode);
	ir_node  *noreg    = ia32_new_NoReg_gp(env_cg);
	ir_node  *nomem    = new_rd_NoMem(irg);
	ir_node  *res;

	if (src_mode == mode_b) {
		assert(mode_is_int(tgt_mode));
		/* nothing to do, we already model bools as 0/1 ints */
		return new_op;
	}

	if (src_mode == tgt_mode) {
		if (get_Conv_strict(node)) {
			if (USE_SSE2(env_cg)) {
				/* when we are in SSE mode, we can kill all strict no-op conversion */
				return new_op;
			}
		} else {
			/* this should be optimized already, but who knows... */
			DEBUG_ONLY(ir_fprintf(stderr, "Debug warning: conv %+F is pointless\n", node));
			DB((dbg, LEVEL_1, "killed Conv(mode, mode) ..."));
			return new_op;
		}
	}

	if (mode_is_float(src_mode)) {
		/* we convert from float ... */
		if (mode_is_float(tgt_mode)) {
			if(src_mode == mode_E && tgt_mode == mode_D
					&& !get_Conv_strict(node)) {
				DB((dbg, LEVEL_1, "killed Conv(mode, mode) ..."));
				return new_op;
			}

			/* ... to float */
			if (USE_SSE2(env_cg)) {
				DB((dbg, LEVEL_1, "create Conv(float, float) ..."));
				res = new_rd_ia32_Conv_FP2FP(dbgi, irg, block, noreg, noreg, new_op, nomem);
				set_ia32_ls_mode(res, tgt_mode);
			} else {
				// Matze: TODO what about strict convs?
				if(get_Conv_strict(node)) {
					res = create_strict_conv(src_mode, tgt_mode, new_op);
					SET_IA32_ORIG_NODE(get_Proj_pred(res), ia32_get_old_node_name(env_cg, node));
					return res;
				}
				DB((dbg, LEVEL_1, "killed Conv(float, float) ..."));
				return new_op;
			}
		} else {
			/* ... to int */
			DB((dbg, LEVEL_1, "create Conv(float, int) ..."));
			if (USE_SSE2(env_cg)) {
				res = new_rd_ia32_Conv_FP2I(dbgi, irg, block, noreg, noreg, new_op, nomem);
				set_ia32_ls_mode(res, src_mode);
			} else {
				return gen_x87_fp_to_gp(node);
			}
		}
	} else {
		/* we convert from int ... */
		if (mode_is_float(tgt_mode)) {
			/* ... to float */
			DB((dbg, LEVEL_1, "create Conv(int, float) ..."));
			if (USE_SSE2(env_cg)) {
				res = new_rd_ia32_Conv_I2FP(dbgi, irg, block, noreg, noreg, new_op, nomem);
				set_ia32_ls_mode(res, tgt_mode);
				if(src_bits == 32) {
					set_ia32_am_support(res, ia32_am_Source, ia32_am_unary);
				}
			} else {
				return gen_x87_gp_to_fp(node, src_mode);
			}
		} else if(tgt_mode == mode_b) {
			/* to bool */
#if 0
			res = create_set(pn_Cmp_Lg, op, NULL, dbgi, block);
#else
			DB((dbg, LEVEL_1, "omitting unnecessary Conv(%+F, %+F) ...", src_mode, tgt_mode));
			return new_op;
#endif
		} else {
			/* to int */
			ir_mode *smaller_mode;
			int     smaller_bits;

			if (src_bits == tgt_bits) {
				DB((dbg, LEVEL_1, "omitting unnecessary Conv(%+F, %+F) ...",
				    src_mode, tgt_mode));
				return new_op;
			}

			if (src_bits < tgt_bits) {
				smaller_mode = src_mode;
				smaller_bits = src_bits;
			} else {
				smaller_mode = tgt_mode;
				smaller_bits = tgt_bits;
			}

			DB((dbg, LEVEL_1, "create Conv(int, int) ...", src_mode, tgt_mode));
			if (smaller_bits == 8) {
				res = new_rd_ia32_Conv_I2I8Bit(dbgi, irg, block, noreg, noreg, new_op, nomem);
				set_ia32_ls_mode(res, smaller_mode);
			} else {
				res = new_rd_ia32_Conv_I2I(dbgi, irg, block, noreg, noreg, new_op, nomem);
				set_ia32_ls_mode(res, smaller_mode);
			}
			set_ia32_am_support(res, ia32_am_Source, ia32_am_unary);
		}
	}

	SET_IA32_ORIG_NODE(res, ia32_get_old_node_name(env_cg, node));

	return res;
}

static
int check_immediate_constraint(long val, char immediate_constraint_type)
{
	switch (immediate_constraint_type) {
	case 0:
		return 1;
	case 'I':
		return val >= 0 && val <= 32;
	case 'J':
		return val >= 0 && val <= 63;
	case 'K':
		return val >= -128 && val <= 127;
	case 'L':
		return val == 0xff || val == 0xffff;
	case 'M':
		return val >= 0 && val <= 3;
	case 'N':
		return val >= 0 && val <= 255;
	case 'O':
		return val >= 0 && val <= 127;
	default:
		break;
	}
	panic("Invalid immediate constraint found");
	return 0;
}

static
ir_node *try_create_Immediate(ir_node *node, char immediate_constraint_type)
{
	int          minus         = 0;
	tarval      *offset        = NULL;
	int          offset_sign   = 0;
	long         val = 0;
	ir_entity   *symconst_ent  = NULL;
	int          symconst_sign = 0;
	ir_mode     *mode;
	ir_node     *cnst          = NULL;
	ir_node     *symconst      = NULL;
	ir_node     *res;
	ir_graph    *irg;
	dbg_info    *dbgi;
	ir_node     *block;

	mode = get_irn_mode(node);
	if(!mode_is_int(mode) && !mode_is_reference(mode)) {
		return NULL;
	}

	if(is_Minus(node)) {
		minus = 1;
		node  = get_Minus_op(node);
	}

	if(is_Const(node)) {
		cnst        = node;
		symconst    = NULL;
		offset_sign = minus;
	} else if(is_SymConst(node)) {
		cnst          = NULL;
		symconst      = node;
		symconst_sign = minus;
	} else if(is_Add(node)) {
		ir_node *left  = get_Add_left(node);
		ir_node *right = get_Add_right(node);
		if(is_Const(left) && is_SymConst(right)) {
			cnst          = left;
			symconst      = right;
			symconst_sign = minus;
			offset_sign   = minus;
		} else if(is_SymConst(left) && is_Const(right)) {
			cnst          = right;
			symconst      = left;
			symconst_sign = minus;
			offset_sign   = minus;
		}
	} else if(is_Sub(node)) {
		ir_node *left  = get_Sub_left(node);
		ir_node *right = get_Sub_right(node);
		if(is_Const(left) && is_SymConst(right)) {
			cnst          = left;
			symconst      = right;
			symconst_sign = !minus;
			offset_sign   = minus;
		} else if(is_SymConst(left) && is_Const(right)) {
			cnst          = right;
			symconst      = left;
			symconst_sign = minus;
			offset_sign   = !minus;
		}
	} else {
		return NULL;
	}

	if(cnst != NULL) {
		offset = get_Const_tarval(cnst);
		if(tarval_is_long(offset)) {
			val = get_tarval_long(offset);
		} else if(tarval_is_null(offset)) {
			val = 0;
		} else {
			ir_fprintf(stderr, "Optimisation Warning: tarval from %+F is not a "
			           "long?\n", cnst);
			return NULL;
		}

		if(!check_immediate_constraint(val, immediate_constraint_type))
			return NULL;
	}
	if(symconst != NULL) {
		if(immediate_constraint_type != 0) {
			/* we need full 32bits for symconsts */
			return NULL;
		}

		if(get_SymConst_kind(symconst) != symconst_addr_ent)
			return NULL;
		symconst_ent = get_SymConst_entity(symconst);
	}
	if(cnst == NULL && symconst == NULL)
		return NULL;

	if(offset_sign && offset != NULL) {
		offset = tarval_neg(offset);
	}

	irg   = current_ir_graph;
	dbgi  = get_irn_dbg_info(node);
	block = get_irg_start_block(irg);
	res   = new_rd_ia32_Immediate(dbgi, irg, block, symconst_ent,
	                              symconst_sign, val);
	arch_set_irn_register(env_cg->arch_env, res, &ia32_gp_regs[REG_GP_NOREG]);

	return res;
}

static
ir_node *create_immediate_or_transform(ir_node *node, char immediate_constraint_type)
{
	ir_node *new_node = try_create_Immediate(node, immediate_constraint_type);
	if (new_node == NULL) {
		new_node = be_transform_node(node);
	}
	return new_node;
}

typedef struct constraint_t constraint_t;
struct constraint_t {
	int                         is_in;
	int                         n_outs;
	const arch_register_req_t **out_reqs;

	const arch_register_req_t  *req;
	unsigned                    immediate_possible;
	char                        immediate_type;
};

void parse_asm_constraint(int pos, constraint_t *constraint, const char *c)
{
	int                          immediate_possible = 0;
	char                         immediate_type     = 0;
	unsigned                     limited            = 0;
	const arch_register_class_t *cls                = NULL;
	ir_graph                    *irg;
	struct obstack              *obst;
	arch_register_req_t         *req;
	unsigned                    *limited_ptr;
	int                          p;
	int                          same_as = -1;

	/* TODO: replace all the asserts with nice error messages */

	printf("Constraint: %s\n", c);

	while(*c != 0) {
		switch(*c) {
		case ' ':
		case '\t':
		case '\n':
			break;

		case 'a':
			assert(cls == NULL ||
					(cls == &ia32_reg_classes[CLASS_ia32_gp] && limited != 0));
			cls      = &ia32_reg_classes[CLASS_ia32_gp];
			limited |= 1 << REG_EAX;
			break;
		case 'b':
			assert(cls == NULL ||
					(cls == &ia32_reg_classes[CLASS_ia32_gp] && limited != 0));
			cls      = &ia32_reg_classes[CLASS_ia32_gp];
			limited |= 1 << REG_EBX;
			break;
		case 'c':
			assert(cls == NULL ||
					(cls == &ia32_reg_classes[CLASS_ia32_gp] && limited != 0));
			cls      = &ia32_reg_classes[CLASS_ia32_gp];
			limited |= 1 << REG_ECX;
			break;
		case 'd':
			assert(cls == NULL ||
					(cls == &ia32_reg_classes[CLASS_ia32_gp] && limited != 0));
			cls      = &ia32_reg_classes[CLASS_ia32_gp];
			limited |= 1 << REG_EDX;
			break;
		case 'D':
			assert(cls == NULL ||
					(cls == &ia32_reg_classes[CLASS_ia32_gp] && limited != 0));
			cls      = &ia32_reg_classes[CLASS_ia32_gp];
			limited |= 1 << REG_EDI;
			break;
		case 'S':
			assert(cls == NULL ||
					(cls == &ia32_reg_classes[CLASS_ia32_gp] && limited != 0));
			cls      = &ia32_reg_classes[CLASS_ia32_gp];
			limited |= 1 << REG_ESI;
			break;
		case 'Q':
		case 'q': /* q means lower part of the regs only, this makes no
				   * difference to Q for us (we only assigne whole registers) */
			assert(cls == NULL ||
					(cls == &ia32_reg_classes[CLASS_ia32_gp] && limited != 0));
			cls      = &ia32_reg_classes[CLASS_ia32_gp];
			limited |= 1 << REG_EAX | 1 << REG_EBX | 1 << REG_ECX |
			           1 << REG_EDX;
			break;
		case 'A':
			assert(cls == NULL ||
					(cls == &ia32_reg_classes[CLASS_ia32_gp] && limited != 0));
			cls      = &ia32_reg_classes[CLASS_ia32_gp];
			limited |= 1 << REG_EAX | 1 << REG_EDX;
			break;
		case 'l':
			assert(cls == NULL ||
					(cls == &ia32_reg_classes[CLASS_ia32_gp] && limited != 0));
			cls      = &ia32_reg_classes[CLASS_ia32_gp];
			limited |= 1 << REG_EAX | 1 << REG_EBX | 1 << REG_ECX |
			           1 << REG_EDX | 1 << REG_ESI | 1 << REG_EDI |
			           1 << REG_EBP;
			break;

		case 'R':
		case 'r':
		case 'p':
			assert(cls == NULL);
			cls      = &ia32_reg_classes[CLASS_ia32_gp];
			break;

		case 'f':
		case 't':
		case 'u':
			/* TODO: mark values so the x87 simulator knows about t and u */
			assert(cls == NULL);
			cls = &ia32_reg_classes[CLASS_ia32_vfp];
			break;

		case 'Y':
		case 'x':
			assert(cls == NULL);
			/* TODO: check that sse2 is supported */
			cls = &ia32_reg_classes[CLASS_ia32_xmm];
			break;

		case 'I':
		case 'J':
		case 'K':
		case 'L':
		case 'M':
		case 'N':
		case 'O':
			assert(!immediate_possible);
			immediate_possible = 1;
			immediate_type     = *c;
			break;
		case 'n':
		case 'i':
			assert(!immediate_possible);
			immediate_possible = 1;
			break;

		case 'g':
			assert(!immediate_possible && cls == NULL);
			immediate_possible = 1;
			cls                = &ia32_reg_classes[CLASS_ia32_gp];
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
			assert(constraint->is_in && "can only specify same constraint "
			       "on input");

			sscanf(c, "%d%n", &same_as, &p);
			if(same_as >= 0) {
				c += p;
				continue;
			}
			break;

		case 'E': /* no float consts yet */
		case 'F': /* no float consts yet */
		case 's': /* makes no sense on x86 */
		case 'X': /* we can't support that in firm */
		case 'm':
		case 'o':
		case 'V':
		case '<': /* no autodecrement on x86 */
		case '>': /* no autoincrement on x86 */
		case 'C': /* sse constant not supported yet */
		case 'G': /* 80387 constant not supported yet */
		case 'y': /* we don't support mmx registers yet */
		case 'Z': /* not available in 32 bit mode */
		case 'e': /* not available in 32 bit mode */
			assert(0 && "asm constraint not supported");
			break;
		default:
			assert(0 && "unknown asm constraint found");
			break;
		}
		++c;
	}

	if(same_as >= 0) {
		const arch_register_req_t *other_constr;

		assert(cls == NULL && "same as and register constraint not supported");
		assert(!immediate_possible && "same as and immediate constraint not "
		       "supported");
		assert(same_as < constraint->n_outs && "wrong constraint number in "
		       "same_as constraint");

		other_constr         = constraint->out_reqs[same_as];

		req                  = obstack_alloc(obst, sizeof(req[0]));
		req->cls             = other_constr->cls;
		req->type            = arch_register_req_type_should_be_same;
		req->limited         = NULL;
		req->other_same      = pos;
		req->other_different = -1;

		/* switch constraints. This is because in firm we have same_as
		 * constraints on the output constraints while in the gcc asm syntax
		 * they are specified on the input constraints */
		constraint->req               = other_constr;
		constraint->out_reqs[same_as] = req;
		constraint->immediate_possible = 0;
		return;
	}

	if(immediate_possible && cls == NULL) {
		cls = &ia32_reg_classes[CLASS_ia32_gp];
	}
	assert(!immediate_possible || cls == &ia32_reg_classes[CLASS_ia32_gp]);
	assert(cls != NULL);

	if(immediate_possible) {
		assert(constraint->is_in
		       && "imeediates make no sense for output constraints");
	}
	/* todo: check types (no float input on 'r' constrainted in and such... */

	irg  = current_ir_graph;
	obst = get_irg_obstack(irg);

	if(limited != 0) {
		req          = obstack_alloc(obst, sizeof(req[0]) + sizeof(unsigned));
		limited_ptr  = (unsigned*) (req+1);
	} else {
		req = obstack_alloc(obst, sizeof(req[0]));
	}
	memset(req, 0, sizeof(req[0]));

	if(limited != 0) {
		req->type    = arch_register_req_type_limited;
		*limited_ptr = limited;
		req->limited = limited_ptr;
	} else {
		req->type    = arch_register_req_type_normal;
	}
	req->cls = cls;

	constraint->req                = req;
	constraint->immediate_possible = immediate_possible;
	constraint->immediate_type     = immediate_type;
}

static
void parse_clobber(ir_node *node, int pos, constraint_t *constraint,
                   const char *c)
{
	(void) node;
	(void) pos;
	(void) constraint;
	(void) c;
	panic("Clobbers not supported yet");
}

ir_node *gen_ASM(ir_node *node)
{
	int                   i, arity;
	ir_graph             *irg   = current_ir_graph;
	ir_node              *block = be_transform_node(get_nodes_block(node));
	dbg_info             *dbgi  = get_irn_dbg_info(node);
	ir_node             **in;
	ir_node              *res;
	int                   out_arity;
	int                   n_outs;
	int                   n_clobbers;
	void                 *generic_attr;
	ia32_asm_attr_t      *attr;
	const arch_register_req_t **out_reqs;
	const arch_register_req_t **in_reqs;
	struct obstack       *obst;
	constraint_t          parsed_constraint;

	/* transform inputs */
	arity = get_irn_arity(node);
	in    = alloca(arity * sizeof(in[0]));
	memset(in, 0, arity * sizeof(in[0]));

	n_outs     = get_ASM_n_output_constraints(node);
	n_clobbers = get_ASM_n_clobbers(node);
	out_arity  = n_outs + n_clobbers;

	/* construct register constraints */
	obst     = get_irg_obstack(irg);
	out_reqs = obstack_alloc(obst, out_arity * sizeof(out_reqs[0]));
	parsed_constraint.out_reqs = out_reqs;
	parsed_constraint.n_outs   = n_outs;
	parsed_constraint.is_in    = 0;
	for(i = 0; i < out_arity; ++i) {
		const char   *c;

		if(i < n_outs) {
			const ir_asm_constraint *constraint;
			constraint = & get_ASM_output_constraints(node) [i];
			c = get_id_str(constraint->constraint);
			parse_asm_constraint(i, &parsed_constraint, c);
		} else {
			ident *glob_id = get_ASM_clobbers(node) [i - n_outs];
			c = get_id_str(glob_id);
			parse_clobber(node, i, &parsed_constraint, c);
		}
		out_reqs[i] = parsed_constraint.req;
	}

	in_reqs = obstack_alloc(obst, arity * sizeof(in_reqs[0]));
	parsed_constraint.is_in = 1;
	for(i = 0; i < arity; ++i) {
		const ir_asm_constraint   *constraint;
		ident                     *constr_id;
		const char                *c;

		constraint = & get_ASM_input_constraints(node) [i];
		constr_id  = constraint->constraint;
		c          = get_id_str(constr_id);
		parse_asm_constraint(i, &parsed_constraint, c);
		in_reqs[i] = parsed_constraint.req;

		if(parsed_constraint.immediate_possible) {
			ir_node *pred      = get_irn_n(node, i);
			char     imm_type  = parsed_constraint.immediate_type;
			ir_node *immediate = try_create_Immediate(pred, imm_type);

			if(immediate != NULL) {
				in[i] = immediate;
			}
		}
	}

	/* transform inputs */
	for(i = 0; i < arity; ++i) {
		ir_node *pred;
		ir_node *transformed;

		if(in[i] != NULL)
			continue;

		pred        = get_irn_n(node, i);
		transformed = be_transform_node(pred);
		in[i]       = transformed;
	}

	res = new_rd_ia32_Asm(dbgi, irg, block, arity, in, out_arity);

	generic_attr   = get_irn_generic_attr(res);
	attr           = CAST_IA32_ATTR(ia32_asm_attr_t, generic_attr);
	attr->asm_text = get_ASM_text(node);
	set_ia32_out_req_all(res, out_reqs);
	set_ia32_in_req_all(res, in_reqs);

	SET_IA32_ORIG_NODE(res, ia32_get_old_node_name(env_cg, node));

	return res;
}

/********************************************
 *  _                          _
 * | |                        | |
 * | |__   ___ _ __   ___   __| | ___  ___
 * | '_ \ / _ \ '_ \ / _ \ / _` |/ _ \/ __|
 * | |_) |  __/ | | | (_) | (_| |  __/\__ \
 * |_.__/ \___|_| |_|\___/ \__,_|\___||___/
 *
 ********************************************/

static ir_node *gen_be_StackParam(ir_node *node) {
	ir_node  *block      = be_transform_node(get_nodes_block(node));
	ir_node   *ptr       = get_irn_n(node, be_pos_StackParam_ptr);
	ir_node   *new_ptr   = be_transform_node(ptr);
	ir_node   *new_op    = NULL;
	ir_graph  *irg       = current_ir_graph;
	dbg_info  *dbgi      = get_irn_dbg_info(node);
	ir_node   *nomem     = new_rd_NoMem(current_ir_graph);
	ir_entity *ent       = arch_get_frame_entity(env_cg->arch_env, node);
	ir_mode   *load_mode = get_irn_mode(node);
	ir_node   *noreg     = ia32_new_NoReg_gp(env_cg);
	ir_mode   *proj_mode;
	long      pn_res;

	if (mode_is_float(load_mode)) {
		if (USE_SSE2(env_cg)) {
			new_op = new_rd_ia32_xLoad(dbgi, irg, block, new_ptr, noreg, nomem);
			pn_res    = pn_ia32_xLoad_res;
			proj_mode = mode_xmm;
		} else {
			new_op = new_rd_ia32_vfld(dbgi, irg, block, new_ptr, noreg, nomem, load_mode);
			pn_res    = pn_ia32_vfld_res;
			proj_mode = mode_vfp;
		}
	} else {
		new_op = new_rd_ia32_Load(dbgi, irg, block, new_ptr, noreg, nomem);
		proj_mode = mode_Iu;
		pn_res = pn_ia32_Load_res;
	}

	set_irn_pinned(new_op, op_pin_state_floats);
	set_ia32_frame_ent(new_op, ent);
	set_ia32_use_frame(new_op);

	set_ia32_op_type(new_op, ia32_AddrModeS);
	set_ia32_am_flavour(new_op, ia32_am_B);
	set_ia32_ls_mode(new_op, load_mode);
	set_ia32_flags(new_op, get_ia32_flags(new_op) | arch_irn_flags_rematerializable);

	SET_IA32_ORIG_NODE(new_op, ia32_get_old_node_name(env_cg, node));

	return new_rd_Proj(dbgi, irg, block, new_op, proj_mode, pn_res);
}

/**
 * Transforms a FrameAddr into an ia32 Add.
 */
static ir_node *gen_be_FrameAddr(ir_node *node) {
	ir_node  *block  = be_transform_node(get_nodes_block(node));
	ir_node  *op     = be_get_FrameAddr_frame(node);
	ir_node  *new_op = be_transform_node(op);
	ir_graph *irg    = current_ir_graph;
	dbg_info *dbgi   = get_irn_dbg_info(node);
	ir_node  *noreg  = ia32_new_NoReg_gp(env_cg);
	ir_node  *res;

	res = new_rd_ia32_Lea(dbgi, irg, block, new_op, noreg);
	set_ia32_frame_ent(res, arch_get_frame_entity(env_cg->arch_env, node));
	set_ia32_use_frame(res);
	set_ia32_am_flavour(res, ia32_am_OB);

	SET_IA32_ORIG_NODE(res, ia32_get_old_node_name(env_cg, node));

	return res;
}

/**
 * Transforms a FrameLoad into an ia32 Load.
 */
static ir_node *gen_be_FrameLoad(ir_node *node) {
	ir_node   *block   = be_transform_node(get_nodes_block(node));
	ir_node   *mem     = get_irn_n(node, be_pos_FrameLoad_mem);
	ir_node   *new_mem = be_transform_node(mem);
	ir_node   *ptr     = get_irn_n(node, be_pos_FrameLoad_ptr);
	ir_node   *new_ptr = be_transform_node(ptr);
	ir_node   *new_op  = NULL;
	ir_graph  *irg     = current_ir_graph;
	dbg_info  *dbgi    = get_irn_dbg_info(node);
	ir_node   *noreg   = ia32_new_NoReg_gp(env_cg);
	ir_entity *ent     = arch_get_frame_entity(env_cg->arch_env, node);
	ir_mode   *mode    = get_type_mode(get_entity_type(ent));
	ir_node   *projs[pn_Load_max];

	ia32_collect_Projs(node, projs, pn_Load_max);

	if (mode_is_float(mode)) {
		if (USE_SSE2(env_cg)) {
			new_op = new_rd_ia32_xLoad(dbgi, irg, block, new_ptr, noreg, new_mem);
		}
		else {
			new_op = new_rd_ia32_vfld(dbgi, irg, block, new_ptr, noreg, new_mem, mode);
		}
	}
	else {
		new_op = new_rd_ia32_Load(dbgi, irg, block, new_ptr, noreg, new_mem);
	}

	set_irn_pinned(new_op, op_pin_state_floats);
	set_ia32_frame_ent(new_op, ent);
	set_ia32_use_frame(new_op);

	set_ia32_op_type(new_op, ia32_AddrModeS);
	set_ia32_am_flavour(new_op, ia32_am_B);
	set_ia32_ls_mode(new_op, mode);
	set_ia32_flags(new_op, get_ia32_flags(new_op) | arch_irn_flags_rematerializable);

	SET_IA32_ORIG_NODE(new_op, ia32_get_old_node_name(env_cg, node));

	return new_op;
}


/**
 * Transforms a FrameStore into an ia32 Store.
 */
static ir_node *gen_be_FrameStore(ir_node *node) {
	ir_node   *block   = be_transform_node(get_nodes_block(node));
	ir_node   *mem     = get_irn_n(node, be_pos_FrameStore_mem);
	ir_node   *new_mem = be_transform_node(mem);
	ir_node   *ptr     = get_irn_n(node, be_pos_FrameStore_ptr);
	ir_node   *new_ptr = be_transform_node(ptr);
	ir_node   *val     = get_irn_n(node, be_pos_FrameStore_val);
	ir_node   *new_val = be_transform_node(val);
	ir_node   *new_op  = NULL;
	ir_graph  *irg     = current_ir_graph;
	dbg_info  *dbgi    = get_irn_dbg_info(node);
	ir_node   *noreg   = ia32_new_NoReg_gp(env_cg);
	ir_entity *ent     = arch_get_frame_entity(env_cg->arch_env, node);
	ir_mode   *mode    = get_irn_mode(val);

	if (mode_is_float(mode)) {
		if (USE_SSE2(env_cg)) {
			new_op = new_rd_ia32_xStore(dbgi, irg, block, new_ptr, noreg, new_val, new_mem);
		} else {
			new_op = new_rd_ia32_vfst(dbgi, irg, block, new_ptr, noreg, new_val, new_mem, mode);
		}
	} else if (get_mode_size_bits(mode) == 8) {
		new_op = new_rd_ia32_Store8Bit(dbgi, irg, block, new_ptr, noreg, new_val, new_mem);
	} else {
		new_op = new_rd_ia32_Store(dbgi, irg, block, new_ptr, noreg, new_val, new_mem);
	}

	set_ia32_frame_ent(new_op, ent);
	set_ia32_use_frame(new_op);

	set_ia32_op_type(new_op, ia32_AddrModeD);
	set_ia32_am_flavour(new_op, ia32_am_B);
	set_ia32_ls_mode(new_op, mode);

	SET_IA32_ORIG_NODE(new_op, ia32_get_old_node_name(env_cg, node));

	return new_op;
}

/**
 * In case SSE is used we need to copy the result from XMM0 to FPU TOS before return.
 */
static ir_node *gen_be_Return(ir_node *node) {
	ir_graph  *irg     = current_ir_graph;
	ir_node   *ret_val = get_irn_n(node, be_pos_Return_val);
	ir_node   *ret_mem = get_irn_n(node, be_pos_Return_mem);
	ir_entity *ent     = get_irg_entity(irg);
	ir_type   *tp      = get_entity_type(ent);
	dbg_info  *dbgi;
	ir_node   *block;
	ir_type   *res_type;
	ir_mode   *mode;
	ir_node   *frame, *sse_store, *fld, *mproj, *barrier;
	ir_node   *new_barrier, *new_ret_val, *new_ret_mem;
	ir_node   *noreg;
	ir_node   **in;
	int       pn_ret_val, pn_ret_mem, arity, i;

	assert(ret_val != NULL);
	if (be_Return_get_n_rets(node) < 1 || ! USE_SSE2(env_cg)) {
		return be_duplicate_node(node);
	}

	res_type = get_method_res_type(tp, 0);

	if (! is_Primitive_type(res_type)) {
		return be_duplicate_node(node);
	}

	mode = get_type_mode(res_type);
	if (! mode_is_float(mode)) {
		return be_duplicate_node(node);
	}

	assert(get_method_n_ress(tp) == 1);

	pn_ret_val = get_Proj_proj(ret_val);
	pn_ret_mem = get_Proj_proj(ret_mem);

	/* get the Barrier */
	barrier = get_Proj_pred(ret_val);

	/* get result input of the Barrier */
	ret_val     = get_irn_n(barrier, pn_ret_val);
	new_ret_val = be_transform_node(ret_val);

	/* get memory input of the Barrier */
	ret_mem     = get_irn_n(barrier, pn_ret_mem);
	new_ret_mem = be_transform_node(ret_mem);

	frame = get_irg_frame(irg);

	dbgi  = get_irn_dbg_info(barrier);
	block = be_transform_node(get_nodes_block(barrier));

	noreg = ia32_new_NoReg_gp(env_cg);

	/* store xmm0 onto stack */
	sse_store = new_rd_ia32_xStoreSimple(dbgi, irg, block, frame, noreg, new_ret_val, new_ret_mem);
	set_ia32_ls_mode(sse_store, mode);
	set_ia32_op_type(sse_store, ia32_AddrModeD);
	set_ia32_use_frame(sse_store);
	set_ia32_am_flavour(sse_store, ia32_am_B);

	/* load into st0 */
	fld = new_rd_ia32_SetST0(dbgi, irg, block, frame, noreg, sse_store);
	set_ia32_ls_mode(fld, mode);
	set_ia32_op_type(fld, ia32_AddrModeS);
	set_ia32_use_frame(fld);
	set_ia32_am_flavour(fld, ia32_am_B);

	mproj = new_r_Proj(irg, block, fld, mode_M, pn_ia32_SetST0_M);
	fld   = new_r_Proj(irg, block, fld, mode_vfp, pn_ia32_SetST0_res);
	arch_set_irn_register(env_cg->arch_env, fld, &ia32_vfp_regs[REG_VF0]);

	/* create a new barrier */
	arity = get_irn_arity(barrier);
	in = alloca(arity * sizeof(in[0]));
	for (i = 0; i < arity; ++i) {
		ir_node *new_in;

		if (i == pn_ret_val) {
			new_in = fld;
		} else if (i == pn_ret_mem) {
			new_in = mproj;
		} else {
			ir_node *in = get_irn_n(barrier, i);
			new_in = be_transform_node(in);
		}
		in[i] = new_in;
	}

	new_barrier = new_ir_node(dbgi, irg, block,
	                          get_irn_op(barrier), get_irn_mode(barrier),
	                          arity, in);
	copy_node_attr(barrier, new_barrier);
	be_duplicate_deps(barrier, new_barrier);
	be_set_transformed_node(barrier, new_barrier);
	mark_irn_visited(barrier);

	/* transform normally */
	return be_duplicate_node(node);
}

/**
 * Transform a be_AddSP into an ia32_AddSP. Eat up const sizes.
 */
static ir_node *gen_be_AddSP(ir_node *node) {
	ir_node  *block  = be_transform_node(get_nodes_block(node));
	ir_node  *sz     = get_irn_n(node, be_pos_AddSP_size);
	ir_node  *new_sz;
	ir_node  *sp     = get_irn_n(node, be_pos_AddSP_old_sp);
	ir_node  *new_sp = be_transform_node(sp);
	ir_graph *irg    = current_ir_graph;
	dbg_info *dbgi   = get_irn_dbg_info(node);
	ir_node  *noreg  = ia32_new_NoReg_gp(env_cg);
	ir_node  *nomem  = new_NoMem();
	ir_node  *new_op;

	new_sz = create_immediate_or_transform(sz, 0);

	/* ia32 stack grows in reverse direction, make a SubSP */
	new_op = new_rd_ia32_SubSP(dbgi, irg, block, noreg, noreg, new_sp, new_sz,
	                           nomem);
	set_ia32_am_support(new_op, ia32_am_Source, ia32_am_binary);
	SET_IA32_ORIG_NODE(new_op, ia32_get_old_node_name(env_cg, node));

	return new_op;
}

/**
 * Transform a be_SubSP into an ia32_SubSP. Eat up const sizes.
 */
static ir_node *gen_be_SubSP(ir_node *node) {
	ir_node  *block  = be_transform_node(get_nodes_block(node));
	ir_node  *sz     = get_irn_n(node, be_pos_SubSP_size);
	ir_node  *new_sz;
	ir_node  *sp     = get_irn_n(node, be_pos_SubSP_old_sp);
	ir_node  *new_sp = be_transform_node(sp);
	ir_graph *irg    = current_ir_graph;
	dbg_info *dbgi   = get_irn_dbg_info(node);
	ir_node  *noreg  = ia32_new_NoReg_gp(env_cg);
	ir_node  *nomem  = new_NoMem();
	ir_node  *new_op;

	new_sz = create_immediate_or_transform(sz, 0);

	/* ia32 stack grows in reverse direction, make an AddSP */
	new_op = new_rd_ia32_AddSP(dbgi, irg, block, noreg, noreg, new_sp, new_sz, nomem);
	set_ia32_am_support(new_op, ia32_am_Source, ia32_am_binary);
	SET_IA32_ORIG_NODE(new_op, ia32_get_old_node_name(env_cg, node));

	return new_op;
}

/**
 * This function just sets the register for the Unknown node
 * as this is not done during register allocation because Unknown
 * is an "ignore" node.
 */
static ir_node *gen_Unknown(ir_node *node) {
	ir_mode *mode = get_irn_mode(node);

	if (mode_is_float(mode)) {
#if 0
		/* Unknown nodes are buggy in x87 sim, use zero for now... */
		if (USE_SSE2(env_cg))
			return ia32_new_Unknown_xmm(env_cg);
		else
			return ia32_new_Unknown_vfp(env_cg);
#else
		ir_graph *irg   = current_ir_graph;
		dbg_info *dbgi  = get_irn_dbg_info(node);
		ir_node  *block = get_irg_start_block(irg);
		return new_rd_ia32_vfldz(dbgi, irg, block);
#endif
	} else if (mode_needs_gp_reg(mode)) {
		return ia32_new_Unknown_gp(env_cg);
	} else {
		assert(0 && "unsupported Unknown-Mode");
	}

	return NULL;
}

/**
 * Change some phi modes
 */
static ir_node *gen_Phi(ir_node *node) {
	ir_node  *block = be_transform_node(get_nodes_block(node));
	ir_graph *irg   = current_ir_graph;
	dbg_info *dbgi  = get_irn_dbg_info(node);
	ir_mode  *mode  = get_irn_mode(node);
	ir_node  *phi;

	if(mode_needs_gp_reg(mode)) {
		/* we shouldn't have any 64bit stuff around anymore */
		assert(get_mode_size_bits(mode) <= 32);
		/* all integer operations are on 32bit registers now */
		mode = mode_Iu;
	} else if(mode_is_float(mode)) {
		if (USE_SSE2(env_cg)) {
			mode = mode_xmm;
		} else {
			mode = mode_vfp;
		}
	}

	/* phi nodes allow loops, so we use the old arguments for now
	 * and fix this later */
	phi = new_ir_node(dbgi, irg, block, op_Phi, mode, get_irn_arity(node), get_irn_in(node) + 1);
	copy_node_attr(node, phi);
	be_duplicate_deps(node, phi);

	be_set_transformed_node(node, phi);
	be_enqueue_preds(node);

	return phi;
}

/**********************************************************************
 *  _                                _                   _
 * | |                              | |                 | |
 * | | _____      _____ _ __ ___  __| |  _ __   ___   __| | ___  ___
 * | |/ _ \ \ /\ / / _ \ '__/ _ \/ _` | | '_ \ / _ \ / _` |/ _ \/ __|
 * | | (_) \ V  V /  __/ | |  __/ (_| | | | | | (_) | (_| |  __/\__ \
 * |_|\___/ \_/\_/ \___|_|  \___|\__,_| |_| |_|\___/ \__,_|\___||___/
 *
 **********************************************************************/

/* These nodes are created in intrinsic lowering (64bit -> 32bit) */

typedef ir_node *construct_load_func(dbg_info *db, ir_graph *irg, ir_node *block, ir_node *base, ir_node *index, \
                                     ir_node *mem);

typedef ir_node *construct_store_func(dbg_info *db, ir_graph *irg, ir_node *block, ir_node *base, ir_node *index, \
                                      ir_node *val, ir_node *mem);

/**
 * Transforms a lowered Load into a "real" one.
 */
static ir_node *gen_lowered_Load(ir_node *node, construct_load_func func)
{
	ir_node  *block   = be_transform_node(get_nodes_block(node));
	ir_node  *ptr     = get_irn_n(node, 0);
	ir_node  *new_ptr = be_transform_node(ptr);
	ir_node  *mem     = get_irn_n(node, 1);
	ir_node  *new_mem = be_transform_node(mem);
	ir_graph *irg     = current_ir_graph;
	dbg_info *dbgi    = get_irn_dbg_info(node);
	ir_mode  *mode    = get_ia32_ls_mode(node);
	ir_node  *noreg   = ia32_new_NoReg_gp(env_cg);
	ir_node  *new_op;

	new_op  = func(dbgi, irg, block, new_ptr, noreg, new_mem);

	set_ia32_op_type(new_op, ia32_AddrModeS);
	set_ia32_am_flavour(new_op, ia32_am_OB);
	set_ia32_am_offs_int(new_op, 0);
	set_ia32_am_scale(new_op, 1);
	set_ia32_am_sc(new_op, get_ia32_am_sc(node));
	if (is_ia32_am_sc_sign(node))
		set_ia32_am_sc_sign(new_op);
	set_ia32_ls_mode(new_op, mode);
	if (is_ia32_use_frame(node)) {
		set_ia32_frame_ent(new_op, get_ia32_frame_ent(node));
		set_ia32_use_frame(new_op);
	}

	SET_IA32_ORIG_NODE(new_op, ia32_get_old_node_name(env_cg, node));

	return new_op;
}

/**
* Transforms a lowered Store into a "real" one.
*/
static ir_node *gen_lowered_Store(ir_node *node, construct_store_func func)
{
	ir_node  *block   = be_transform_node(get_nodes_block(node));
	ir_node  *ptr     = get_irn_n(node, 0);
	ir_node  *new_ptr = be_transform_node(ptr);
	ir_node  *val     = get_irn_n(node, 1);
	ir_node  *new_val = be_transform_node(val);
	ir_node  *mem     = get_irn_n(node, 2);
	ir_node  *new_mem = be_transform_node(mem);
	ir_graph *irg     = current_ir_graph;
	dbg_info *dbgi    = get_irn_dbg_info(node);
	ir_node  *noreg   = ia32_new_NoReg_gp(env_cg);
	ir_mode  *mode    = get_ia32_ls_mode(node);
	ir_node  *new_op;
	long     am_offs;
	ia32_am_flavour_t am_flav = ia32_B;

	new_op = func(dbgi, irg, block, new_ptr, noreg, new_val, new_mem);

	if ((am_offs = get_ia32_am_offs_int(node)) != 0) {
		am_flav |= ia32_O;
		add_ia32_am_offs_int(new_op, am_offs);
	}

	set_ia32_op_type(new_op, ia32_AddrModeD);
	set_ia32_am_flavour(new_op, am_flav);
	set_ia32_ls_mode(new_op, mode);
	set_ia32_frame_ent(new_op, get_ia32_frame_ent(node));
	set_ia32_use_frame(new_op);

	SET_IA32_ORIG_NODE(new_op, ia32_get_old_node_name(env_cg, node));

	return new_op;
}


/**
 * Transforms an ia32_l_XXX into a "real" XXX node
 *
 * @param env   The transformation environment
 * @return the created ia32 XXX node
 */
#define GEN_LOWERED_OP(op)                                                \
	static ir_node *gen_ia32_l_##op(ir_node *node) {                      \
		return gen_binop(node, get_binop_left(node),                      \
		                 get_binop_right(node), new_rd_ia32_##op,0);      \
	}

#define GEN_LOWERED_x87_OP(op)                                                 \
	static ir_node *gen_ia32_l_##op(ir_node *node) {                           \
		ir_node *new_op;                                                       \
		new_op = gen_binop_x87_float(node, get_binop_left(node),               \
		                             get_binop_right(node), new_rd_ia32_##op); \
		return new_op;                                                         \
	}

#define GEN_LOWERED_UNOP(op)                                                   \
	static ir_node *gen_ia32_l_##op(ir_node *node) {\
		return gen_unop(node, get_unop_op(node), new_rd_ia32_##op);       \
	}

#define GEN_LOWERED_SHIFT_OP(op)                                               \
	static ir_node *gen_ia32_l_##op(ir_node *node) {\
		return gen_shift_binop(node, get_binop_left(node),                \
		                       get_binop_right(node), new_rd_ia32_##op);       \
	}

#define GEN_LOWERED_LOAD(op)                                   \
	static ir_node *gen_ia32_l_##op(ir_node *node) {           \
		return gen_lowered_Load(node, new_rd_ia32_##op);       \
	}

#define GEN_LOWERED_STORE(op)                                \
	static ir_node *gen_ia32_l_##op(ir_node *node) {         \
		return gen_lowered_Store(node, new_rd_ia32_##op);    \
	}

GEN_LOWERED_OP(Adc)
GEN_LOWERED_OP(Add)
GEN_LOWERED_OP(Sbb)
GEN_LOWERED_OP(Sub)
GEN_LOWERED_OP(IMul)
GEN_LOWERED_OP(Xor)
GEN_LOWERED_x87_OP(vfprem)
GEN_LOWERED_x87_OP(vfmul)
GEN_LOWERED_x87_OP(vfsub)

GEN_LOWERED_UNOP(Neg)

GEN_LOWERED_LOAD(vfild)
GEN_LOWERED_LOAD(Load)
// GEN_LOWERED_STORE(vfist) TODO
GEN_LOWERED_STORE(Store)

static ir_node *gen_ia32_l_vfdiv(ir_node *node) {
	ir_node  *block     = be_transform_node(get_nodes_block(node));
	ir_node  *left      = get_binop_left(node);
	ir_node  *new_left  = be_transform_node(left);
	ir_node  *right     = get_binop_right(node);
	ir_node  *new_right = be_transform_node(right);
	ir_node  *noreg     = ia32_new_NoReg_gp(env_cg);
	ir_graph *irg       = current_ir_graph;
	dbg_info *dbgi      = get_irn_dbg_info(node);
	ir_node  *fpcw      = be_abi_get_ignore_irn(env_cg->birg->abi,
	                                            &ia32_fp_cw_regs[REG_FPCW]);
	ir_node  *vfdiv;

	vfdiv = new_rd_ia32_vfdiv(dbgi, irg, block, noreg, noreg, new_left,
	                          new_right, new_NoMem(), fpcw);
	clear_ia32_commutative(vfdiv);
	set_ia32_am_support(vfdiv, ia32_am_Source, ia32_am_binary);

	SET_IA32_ORIG_NODE(vfdiv, ia32_get_old_node_name(env_cg, node));

	return vfdiv;
}

/**
 * Transforms a l_MulS into a "real" MulS node.
 *
 * @param env   The transformation environment
 * @return the created ia32 Mul node
 */
static ir_node *gen_ia32_l_Mul(ir_node *node) {
	ir_node  *block     = be_transform_node(get_nodes_block(node));
	ir_node  *left      = get_binop_left(node);
	ir_node  *new_left  = be_transform_node(left);
	ir_node  *right     = get_binop_right(node);
	ir_node  *new_right = be_transform_node(right);
	ir_node  *noreg     = ia32_new_NoReg_gp(env_cg);
	ir_graph *irg       = current_ir_graph;
	dbg_info *dbgi      = get_irn_dbg_info(node);

	/* l_Mul is already a mode_T node, so we create the Mul in the normal way   */
	/* and then skip the result Proj, because all needed Projs are already there. */
	ir_node *muls = new_rd_ia32_Mul(dbgi, irg, block, noreg, noreg, new_left,
	                                new_right, new_NoMem());
	clear_ia32_commutative(muls);
	set_ia32_am_support(muls, ia32_am_Source, ia32_am_binary);

	SET_IA32_ORIG_NODE(muls, ia32_get_old_node_name(env_cg, node));

	return muls;
}

GEN_LOWERED_SHIFT_OP(Shl)
GEN_LOWERED_SHIFT_OP(Shr)
GEN_LOWERED_SHIFT_OP(Sar)

/**
 * Transforms a l_ShlD/l_ShrD into a ShlD/ShrD. Those nodes have 3 data inputs:
 * op1 - target to be shifted
 * op2 - contains bits to be shifted into target
 * op3 - shift count
 * Only op3 can be an immediate.
 */
static ir_node *gen_lowered_64bit_shifts(ir_node *node, ir_node *op1,
                                         ir_node *op2, ir_node *count)
{
	ir_node  *block     = be_transform_node(get_nodes_block(node));
	ir_node  *new_op1   = be_transform_node(op1);
	ir_node  *new_op2   = be_transform_node(op2);
	ir_node  *new_count = be_transform_node(count);
	ir_node  *new_op    = NULL;
	ir_graph *irg       = current_ir_graph;
	dbg_info *dbgi      = get_irn_dbg_info(node);
	ir_node  *noreg     = ia32_new_NoReg_gp(env_cg);
	ir_node  *nomem     = new_NoMem();
	ir_node  *imm_op;
	tarval   *tv;

	assert(! mode_is_float(get_irn_mode(node)) && "Shift/Rotate with float not supported");

	/* Check if immediate optimization is on and */
	/* if it's an operation with immediate.      */
	imm_op  = (env_cg->opt & IA32_OPT_IMMOPS) ? get_immediate_op(NULL, new_count) : NULL;

	/* Limit imm_op within range imm8 */
	if (imm_op) {
		tv = get_ia32_Immop_tarval(imm_op);

		if (tv) {
			tv = tarval_mod(tv, new_tarval_from_long(32, get_tarval_mode(tv)));
			set_ia32_Immop_tarval(imm_op, tv);
		}
		else {
			imm_op = NULL;
		}
	}

	/* integer operations */
	if (imm_op) {
		/* This is ShiftD with const */
		DB((dbg, LEVEL_1, "ShiftD with immediate ..."));

		if (is_ia32_l_ShlD(node))
			new_op = new_rd_ia32_ShlD(dbgi, irg, block, noreg, noreg,
			                          new_op1, new_op2, noreg, nomem);
		else
			new_op = new_rd_ia32_ShrD(dbgi, irg, block, noreg, noreg,
			                          new_op1, new_op2, noreg, nomem);
		copy_ia32_Immop_attr(new_op, imm_op);
	}
	else {
		/* This is a normal ShiftD */
		DB((dbg, LEVEL_1, "ShiftD binop ..."));
		if (is_ia32_l_ShlD(node))
			new_op = new_rd_ia32_ShlD(dbgi, irg, block, noreg, noreg,
			                          new_op1, new_op2, new_count, nomem);
		else
			new_op = new_rd_ia32_ShrD(dbgi, irg, block, noreg, noreg,
			                          new_op1, new_op2, new_count, nomem);
	}

	/* set AM support */
	set_ia32_am_support(new_op, ia32_am_Dest, ia32_am_binary);

	SET_IA32_ORIG_NODE(new_op, ia32_get_old_node_name(env_cg, node));

	set_ia32_emit_cl(new_op);

	return new_op;
}

static ir_node *gen_ia32_l_ShlD(ir_node *node) {
	return gen_lowered_64bit_shifts(node, get_irn_n(node, 0),
	                                get_irn_n(node, 1), get_irn_n(node, 2));
}

static ir_node *gen_ia32_l_ShrD(ir_node *node) {
	return gen_lowered_64bit_shifts(node, get_irn_n(node, 0),
	                                get_irn_n(node, 1), get_irn_n(node, 2));
}

/**
 * In case SSE Unit is used, the node is transformed into a vfst + xLoad.
 */
static ir_node *gen_ia32_l_X87toSSE(ir_node *node) {
	ir_node         *block   = be_transform_node(get_nodes_block(node));
	ir_node         *val     = get_irn_n(node, 1);
	ir_node         *new_val = be_transform_node(val);
	ia32_code_gen_t *cg      = env_cg;
	ir_node         *res     = NULL;
	ir_graph        *irg     = current_ir_graph;
	dbg_info        *dbgi;
	ir_node         *noreg, *new_ptr, *new_mem;
	ir_node         *ptr, *mem;

	if (USE_SSE2(cg)) {
		return new_val;
	}

	mem     = get_irn_n(node, 2);
	new_mem = be_transform_node(mem);
	ptr     = get_irn_n(node, 0);
	new_ptr = be_transform_node(ptr);
	noreg   = ia32_new_NoReg_gp(cg);
	dbgi    = get_irn_dbg_info(node);

	/* Store x87 -> MEM */
	res = new_rd_ia32_vfst(dbgi, irg, block, new_ptr, noreg, new_val, new_mem, get_ia32_ls_mode(node));
	set_ia32_frame_ent(res, get_ia32_frame_ent(node));
	set_ia32_use_frame(res);
	set_ia32_ls_mode(res, get_ia32_ls_mode(node));
	set_ia32_am_flavour(res, ia32_B);
	set_ia32_op_type(res, ia32_AddrModeD);

	/* Load MEM -> SSE */
	res = new_rd_ia32_xLoad(dbgi, irg, block, new_ptr, noreg, res);
	set_ia32_frame_ent(res, get_ia32_frame_ent(node));
	set_ia32_use_frame(res);
	set_ia32_ls_mode(res, get_ia32_ls_mode(node));
	set_ia32_am_flavour(res, ia32_B);
	set_ia32_op_type(res, ia32_AddrModeS);
	res = new_rd_Proj(dbgi, irg, block, res, mode_xmm, pn_ia32_xLoad_res);

	return res;
}

/**
 * In case SSE Unit is used, the node is transformed into a xStore + vfld.
 */
static ir_node *gen_ia32_l_SSEtoX87(ir_node *node) {
	ir_node         *block   = be_transform_node(get_nodes_block(node));
	ir_node         *val     = get_irn_n(node, 1);
	ir_node         *new_val = be_transform_node(val);
	ia32_code_gen_t *cg      = env_cg;
	ir_graph        *irg     = current_ir_graph;
	ir_node         *res     = NULL;
	ir_entity       *fent    = get_ia32_frame_ent(node);
	ir_mode         *lsmode  = get_ia32_ls_mode(node);
	int             offs     = 0;
	ir_node         *noreg, *new_ptr, *new_mem;
	ir_node         *ptr, *mem;
	dbg_info        *dbgi;

	if (! USE_SSE2(cg)) {
		/* SSE unit is not used -> skip this node. */
		return new_val;
	}

	ptr     = get_irn_n(node, 0);
	new_ptr = be_transform_node(ptr);
	mem     = get_irn_n(node, 2);
	new_mem = be_transform_node(mem);
	noreg   = ia32_new_NoReg_gp(cg);
	dbgi    = get_irn_dbg_info(node);

	/* Store SSE -> MEM */
	if (is_ia32_xLoad(skip_Proj(new_val))) {
		ir_node *ld = skip_Proj(new_val);

		/* we can vfld the value directly into the fpu */
		fent = get_ia32_frame_ent(ld);
		ptr  = get_irn_n(ld, 0);
		offs = get_ia32_am_offs_int(ld);
	} else {
		res = new_rd_ia32_xStore(dbgi, irg, block, new_ptr, noreg, new_val, new_mem);
		set_ia32_frame_ent(res, fent);
		set_ia32_use_frame(res);
		set_ia32_ls_mode(res, lsmode);
		set_ia32_am_flavour(res, ia32_B);
		set_ia32_op_type(res, ia32_AddrModeD);
		mem = res;
	}

	/* Load MEM -> x87 */
	res = new_rd_ia32_vfld(dbgi, irg, block, new_ptr, noreg, new_mem, lsmode);
	set_ia32_frame_ent(res, fent);
	set_ia32_use_frame(res);
	add_ia32_am_offs_int(res, offs);
	set_ia32_am_flavour(res, ia32_B);
	set_ia32_op_type(res, ia32_AddrModeS);
	res = new_rd_Proj(dbgi, irg, block, res, mode_vfp, pn_ia32_vfld_res);

	return res;
}

/*********************************************************
 *                  _             _      _
 *                 (_)           | |    (_)
 *  _ __ ___   __ _ _ _ __     __| |_ __ ___   _____ _ __
 * | '_ ` _ \ / _` | | '_ \   / _` | '__| \ \ / / _ \ '__|
 * | | | | | | (_| | | | | | | (_| | |  | |\ V /  __/ |
 * |_| |_| |_|\__,_|_|_| |_|  \__,_|_|  |_| \_/ \___|_|
 *
 *********************************************************/

/**
 * the BAD transformer.
 */
static ir_node *bad_transform(ir_node *node) {
	panic("No transform function for %+F available.\n", node);
	return NULL;
}

/**
 * Transform the Projs of an AddSP.
 */
static ir_node *gen_Proj_be_AddSP(ir_node *node) {
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_node  *pred     = get_Proj_pred(node);
	ir_node  *new_pred = be_transform_node(pred);
	ir_graph *irg      = current_ir_graph;
	dbg_info *dbgi     = get_irn_dbg_info(node);
	long     proj      = get_Proj_proj(node);

	if (proj == pn_be_AddSP_sp) {
		ir_node *res = new_rd_Proj(dbgi, irg, block, new_pred, mode_Iu,
		                           pn_ia32_SubSP_stack);
		arch_set_irn_register(env_cg->arch_env, res, &ia32_gp_regs[REG_ESP]);
		return res;
	} else if(proj == pn_be_AddSP_res) {
		return new_rd_Proj(dbgi, irg, block, new_pred, mode_Iu,
		                   pn_ia32_SubSP_addr);
	} else if (proj == pn_be_AddSP_M) {
		return new_rd_Proj(dbgi, irg, block, new_pred, mode_M, pn_ia32_SubSP_M);
	}

	assert(0);
	return new_rd_Unknown(irg, get_irn_mode(node));
}

/**
 * Transform the Projs of a SubSP.
 */
static ir_node *gen_Proj_be_SubSP(ir_node *node) {
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_node  *pred     = get_Proj_pred(node);
	ir_node  *new_pred = be_transform_node(pred);
	ir_graph *irg      = current_ir_graph;
	dbg_info *dbgi     = get_irn_dbg_info(node);
	long     proj      = get_Proj_proj(node);

	if (proj == pn_be_SubSP_sp) {
		ir_node *res = new_rd_Proj(dbgi, irg, block, new_pred, mode_Iu,
		                           pn_ia32_AddSP_stack);
		arch_set_irn_register(env_cg->arch_env, res, &ia32_gp_regs[REG_ESP]);
		return res;
	} else if (proj == pn_be_SubSP_M) {
		return new_rd_Proj(dbgi, irg, block, new_pred, mode_M, pn_ia32_AddSP_M);
	}

	assert(0);
	return new_rd_Unknown(irg, get_irn_mode(node));
}

/**
 * Transform and renumber the Projs from a Load.
 */
static ir_node *gen_Proj_Load(ir_node *node) {
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_node  *pred     = get_Proj_pred(node);
	ir_node  *new_pred = be_transform_node(pred);
	ir_graph *irg      = current_ir_graph;
	dbg_info *dbgi     = get_irn_dbg_info(node);
	long     proj      = get_Proj_proj(node);

	/* renumber the proj */
	if (is_ia32_Load(new_pred)) {
		if (proj == pn_Load_res) {
			return new_rd_Proj(dbgi, irg, block, new_pred, mode_Iu, pn_ia32_Load_res);
		} else if (proj == pn_Load_M) {
			return new_rd_Proj(dbgi, irg, block, new_pred, mode_M, pn_ia32_Load_M);
		}
	} else if (is_ia32_xLoad(new_pred)) {
		if (proj == pn_Load_res) {
			return new_rd_Proj(dbgi, irg, block, new_pred, mode_xmm, pn_ia32_xLoad_res);
		} else if (proj == pn_Load_M) {
			return new_rd_Proj(dbgi, irg, block, new_pred, mode_M, pn_ia32_xLoad_M);
		}
	} else if (is_ia32_vfld(new_pred)) {
		if (proj == pn_Load_res) {
			return new_rd_Proj(dbgi, irg, block, new_pred, mode_vfp, pn_ia32_vfld_res);
		} else if (proj == pn_Load_M) {
			return new_rd_Proj(dbgi, irg, block, new_pred, mode_M, pn_ia32_vfld_M);
		}
	}

	assert(0);
	return new_rd_Unknown(irg, get_irn_mode(node));
}

/**
 * Transform and renumber the Projs from a DivMod like instruction.
 */
static ir_node *gen_Proj_DivMod(ir_node *node) {
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_node  *pred     = get_Proj_pred(node);
	ir_node  *new_pred = be_transform_node(pred);
	ir_graph *irg      = current_ir_graph;
	dbg_info *dbgi     = get_irn_dbg_info(node);
	ir_mode  *mode     = get_irn_mode(node);
	long     proj      = get_Proj_proj(node);

	assert(is_ia32_Div(new_pred) || is_ia32_IDiv(new_pred));

	switch (get_irn_opcode(pred)) {
	case iro_Div:
		switch (proj) {
		case pn_Div_M:
			return new_rd_Proj(dbgi, irg, block, new_pred, mode_M, pn_ia32_Div_M);
		case pn_Div_res:
			return new_rd_Proj(dbgi, irg, block, new_pred, mode_Iu, pn_ia32_Div_div_res);
		default:
			break;
		}
		break;
	case iro_Mod:
		switch (proj) {
		case pn_Mod_M:
			return new_rd_Proj(dbgi, irg, block, new_pred, mode_M, pn_ia32_Div_M);
		case pn_Mod_res:
			return new_rd_Proj(dbgi, irg, block, new_pred, mode_Iu, pn_ia32_Div_mod_res);
		default:
			break;
		}
		break;
	case iro_DivMod:
		switch (proj) {
		case pn_DivMod_M:
			return new_rd_Proj(dbgi, irg, block, new_pred, mode_M, pn_ia32_Div_M);
		case pn_DivMod_res_div:
			return new_rd_Proj(dbgi, irg, block, new_pred, mode_Iu, pn_ia32_Div_div_res);
		case pn_DivMod_res_mod:
			return new_rd_Proj(dbgi, irg, block, new_pred, mode_Iu, pn_ia32_Div_mod_res);
		default:
			break;
		}
		break;
	default:
		break;
	}

	assert(0);
	return new_rd_Unknown(irg, mode);
}

/**
 * Transform and renumber the Projs from a CopyB.
 */
static ir_node *gen_Proj_CopyB(ir_node *node) {
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_node  *pred     = get_Proj_pred(node);
	ir_node  *new_pred = be_transform_node(pred);
	ir_graph *irg      = current_ir_graph;
	dbg_info *dbgi     = get_irn_dbg_info(node);
	ir_mode  *mode     = get_irn_mode(node);
	long     proj      = get_Proj_proj(node);

	switch(proj) {
	case pn_CopyB_M_regular:
		if (is_ia32_CopyB_i(new_pred)) {
			return new_rd_Proj(dbgi, irg, block, new_pred, mode_M, pn_ia32_CopyB_i_M);
		} else if (is_ia32_CopyB(new_pred)) {
			return new_rd_Proj(dbgi, irg, block, new_pred, mode_M, pn_ia32_CopyB_M);
		}
		break;
	default:
		break;
	}

	assert(0);
	return new_rd_Unknown(irg, mode);
}

/**
 * Transform and renumber the Projs from a vfdiv.
 */
static ir_node *gen_Proj_l_vfdiv(ir_node *node) {
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_node  *pred     = get_Proj_pred(node);
	ir_node  *new_pred = be_transform_node(pred);
	ir_graph *irg      = current_ir_graph;
	dbg_info *dbgi     = get_irn_dbg_info(node);
	ir_mode  *mode     = get_irn_mode(node);
	long     proj      = get_Proj_proj(node);

	switch (proj) {
	case pn_ia32_l_vfdiv_M:
		return new_rd_Proj(dbgi, irg, block, new_pred, mode_M, pn_ia32_vfdiv_M);
	case pn_ia32_l_vfdiv_res:
		return new_rd_Proj(dbgi, irg, block, new_pred, mode_vfp, pn_ia32_vfdiv_res);
	default:
		assert(0);
	}

	return new_rd_Unknown(irg, mode);
}

/**
 * Transform and renumber the Projs from a Quot.
 */
static ir_node *gen_Proj_Quot(ir_node *node) {
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_node  *pred     = get_Proj_pred(node);
	ir_node  *new_pred = be_transform_node(pred);
	ir_graph *irg      = current_ir_graph;
	dbg_info *dbgi     = get_irn_dbg_info(node);
	ir_mode  *mode     = get_irn_mode(node);
	long     proj      = get_Proj_proj(node);

	switch(proj) {
	case pn_Quot_M:
		if (is_ia32_xDiv(new_pred)) {
			return new_rd_Proj(dbgi, irg, block, new_pred, mode_M, pn_ia32_xDiv_M);
		} else if (is_ia32_vfdiv(new_pred)) {
			return new_rd_Proj(dbgi, irg, block, new_pred, mode_M, pn_ia32_vfdiv_M);
		}
		break;
	case pn_Quot_res:
		if (is_ia32_xDiv(new_pred)) {
			return new_rd_Proj(dbgi, irg, block, new_pred, mode_xmm, pn_ia32_xDiv_res);
		} else if (is_ia32_vfdiv(new_pred)) {
			return new_rd_Proj(dbgi, irg, block, new_pred, mode_vfp, pn_ia32_vfdiv_res);
		}
		break;
	default:
		break;
	}

	assert(0);
	return new_rd_Unknown(irg, mode);
}

/**
 * Transform the Thread Local Storage Proj.
 */
static ir_node *gen_Proj_tls(ir_node *node) {
	ir_node  *block = be_transform_node(get_nodes_block(node));
	ir_graph *irg   = current_ir_graph;
	dbg_info *dbgi  = NULL;
	ir_node  *res   = new_rd_ia32_LdTls(dbgi, irg, block, mode_Iu);

	return res;
}

/**
 * Transform the Projs from a be_Call.
 */
static ir_node *gen_Proj_be_Call(ir_node *node) {
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_node  *call     = get_Proj_pred(node);
	ir_node  *new_call = be_transform_node(call);
	ir_graph *irg      = current_ir_graph;
	dbg_info *dbgi     = get_irn_dbg_info(node);
	long     proj      = get_Proj_proj(node);
	ir_mode  *mode     = get_irn_mode(node);
	ir_node  *sse_load;
	const arch_register_class_t *cls;

	/* The following is kinda tricky: If we're using SSE, then we have to
	 * move the result value of the call in floating point registers to an
	 * xmm register, we therefore construct a GetST0 -> xLoad sequence
	 * after the call, we have to make sure to correctly make the
	 * MemProj and the result Proj use these 2 nodes
	 */
	if (proj == pn_be_Call_M_regular) {
		// get new node for result, are we doing the sse load/store hack?
		ir_node *call_res = be_get_Proj_for_pn(call, pn_be_Call_first_res);
		ir_node *call_res_new;
		ir_node *call_res_pred = NULL;

		if (call_res != NULL) {
			call_res_new  = be_transform_node(call_res);
			call_res_pred = get_Proj_pred(call_res_new);
		}

		if (call_res_pred == NULL || be_is_Call(call_res_pred)) {
			return new_rd_Proj(dbgi, irg, block, new_call, mode_M, pn_be_Call_M_regular);
		} else {
			assert(is_ia32_xLoad(call_res_pred));
			return new_rd_Proj(dbgi, irg, block, call_res_pred, mode_M, pn_ia32_xLoad_M);
		}
	}
	if (proj == pn_be_Call_first_res && mode_is_float(mode) && USE_SSE2(env_cg)) {
		ir_node *fstp;
		ir_node *frame = get_irg_frame(irg);
		ir_node *noreg = ia32_new_NoReg_gp(env_cg);
		ir_node *p;
		ir_node *call_mem = be_get_Proj_for_pn(call, pn_be_Call_M_regular);

		/* in case there is no memory output: create one to serialize the copy FPU -> SSE */
		call_mem = new_rd_Proj(dbgi, irg, block, new_call, mode_M, pn_be_Call_M_regular);

		/* store st(0) onto stack */
		fstp = new_rd_ia32_GetST0(dbgi, irg, block, frame, noreg, call_mem);

		set_ia32_ls_mode(fstp, mode);
		set_ia32_op_type(fstp, ia32_AddrModeD);
		set_ia32_use_frame(fstp);
		set_ia32_am_flavour(fstp, ia32_am_B);

		/* load into SSE register */
		sse_load = new_rd_ia32_xLoad(dbgi, irg, block, frame, noreg, fstp);
		set_ia32_ls_mode(sse_load, mode);
		set_ia32_op_type(sse_load, ia32_AddrModeS);
		set_ia32_use_frame(sse_load);
		set_ia32_am_flavour(sse_load, ia32_am_B);

		sse_load = new_rd_Proj(dbgi, irg, block, sse_load, mode_xmm, pn_ia32_xLoad_res);

		/* now: create new Keep whith all former ins and one additional in - the result Proj */

		/* get a Proj representing a caller save register */
		p = be_get_Proj_for_pn(call, pn_be_Call_first_res + 1);
		assert(is_Proj(p) && "Proj expected.");

		/* user of the the proj is the Keep */
		p = get_edge_src_irn(get_irn_out_edge_first(p));
		assert(be_is_Keep(p) && "Keep expected.");

		return sse_load;
	}

	/* transform call modes */
	if (mode_is_data(mode)) {
		cls = arch_get_irn_reg_class(env_cg->arch_env, node, -1);
		mode = cls->mode;
	}

	return new_rd_Proj(dbgi, irg, block, new_call, mode, proj);
}

/**
 * Transform the Projs from a Cmp.
 */
static ir_node *gen_Proj_Cmp(ir_node *node)
{
	/* normally Cmps are processed when looking at Cond nodes, but this case
	 * can happen in complicated Psi conditions */

	ir_node  *cmp       = get_Proj_pred(node);
	long      pnc       = get_Proj_proj(node);
	ir_node  *cmp_left  = get_Cmp_left(cmp);
	ir_node  *cmp_right = get_Cmp_right(cmp);
	ir_mode  *cmp_mode  = get_irn_mode(cmp_left);
	dbg_info *dbgi      = get_irn_dbg_info(cmp);
	ir_node  *block     = be_transform_node(get_nodes_block(node));
	ir_node  *res;

	assert(!mode_is_float(cmp_mode));

	if(!mode_is_signed(cmp_mode)) {
		pnc |= ia32_pn_Cmp_Unsigned;
	}

	res = create_set(pnc, cmp_left, cmp_right, dbgi, block);
	SET_IA32_ORIG_NODE(res, ia32_get_old_node_name(env_cg, cmp));

	return res;
}

/**
 * Transform and potentially renumber Proj nodes.
 */
static ir_node *gen_Proj(ir_node *node) {
	ir_graph *irg  = current_ir_graph;
	dbg_info *dbgi = get_irn_dbg_info(node);
	ir_node  *pred = get_Proj_pred(node);
	long     proj  = get_Proj_proj(node);

	if (is_Store(pred) || be_is_FrameStore(pred)) {
		if (proj == pn_Store_M) {
			return be_transform_node(pred);
		} else {
			assert(0);
			return new_r_Bad(irg);
		}
	} else if (is_Load(pred) || be_is_FrameLoad(pred)) {
		return gen_Proj_Load(node);
	} else if (is_Div(pred) || is_Mod(pred) || is_DivMod(pred)) {
		return gen_Proj_DivMod(node);
	} else if (is_CopyB(pred)) {
		return gen_Proj_CopyB(node);
	} else if (is_Quot(pred)) {
		return gen_Proj_Quot(node);
	} else if (is_ia32_l_vfdiv(pred)) {
		return gen_Proj_l_vfdiv(node);
	} else if (be_is_SubSP(pred)) {
		return gen_Proj_be_SubSP(node);
	} else if (be_is_AddSP(pred)) {
		return gen_Proj_be_AddSP(node);
	} else if (be_is_Call(pred)) {
		return gen_Proj_be_Call(node);
	} else if (is_Cmp(pred)) {
		return gen_Proj_Cmp(node);
	} else if (get_irn_op(pred) == op_Start) {
		if (proj == pn_Start_X_initial_exec) {
			ir_node *block = get_nodes_block(pred);
			ir_node *jump;

			/* we exchange the ProjX with a jump */
			block = be_transform_node(block);
			jump  = new_rd_Jmp(dbgi, irg, block);
			return jump;
		}
		if (node == be_get_old_anchor(anchor_tls)) {
			return gen_Proj_tls(node);
		}
	} else {
		ir_node *new_pred = be_transform_node(pred);
		ir_node *block    = be_transform_node(get_nodes_block(node));
		ir_mode *mode     = get_irn_mode(node);
		if (mode_needs_gp_reg(mode)) {
			ir_node *new_proj = new_r_Proj(irg, block, new_pred, mode_Iu,
			                               get_Proj_proj(node));
#ifdef DEBUG_libfirm
			new_proj->node_nr = node->node_nr;
#endif
			return new_proj;
		}
	}

	return be_duplicate_node(node);
}

/**
 * Enters all transform functions into the generic pointer
 */
static void register_transformers(void)
{
	ir_op *op_Mulh;

	/* first clear the generic function pointer for all ops */
	clear_irp_opcodes_generic_func();

#define GEN(a)   { be_transform_func *func = gen_##a; op_##a->ops.generic = (op_func) func; }
#define BAD(a)   op_##a->ops.generic = (op_func)bad_transform

	GEN(Add);
	GEN(Sub);
	GEN(Mul);
	GEN(And);
	GEN(Or);
	GEN(Eor);

	GEN(Shl);
	GEN(Shr);
	GEN(Shrs);
	GEN(Rot);

	GEN(Quot);

	GEN(Div);
	GEN(Mod);
	GEN(DivMod);

	GEN(Minus);
	GEN(Conv);
	GEN(Abs);
	GEN(Not);

	GEN(Load);
	GEN(Store);
	GEN(Cond);

	GEN(ASM);
	GEN(CopyB);
	BAD(Mux);
	GEN(Psi);
	GEN(Proj);
	GEN(Phi);

	/* transform ops from intrinsic lowering */
	GEN(ia32_l_Add);
	GEN(ia32_l_Adc);
	GEN(ia32_l_Sub);
	GEN(ia32_l_Sbb);
	GEN(ia32_l_Neg);
	GEN(ia32_l_Mul);
	GEN(ia32_l_Xor);
	GEN(ia32_l_IMul);
	GEN(ia32_l_Shl);
	GEN(ia32_l_Shr);
	GEN(ia32_l_Sar);
	GEN(ia32_l_ShlD);
	GEN(ia32_l_ShrD);
	GEN(ia32_l_vfdiv);
	GEN(ia32_l_vfprem);
	GEN(ia32_l_vfmul);
	GEN(ia32_l_vfsub);
	GEN(ia32_l_vfild);
	GEN(ia32_l_Load);
	/* GEN(ia32_l_vfist); TODO */
	GEN(ia32_l_Store);
	GEN(ia32_l_X87toSSE);
	GEN(ia32_l_SSEtoX87);

	GEN(Const);
	GEN(SymConst);

	/* we should never see these nodes */
	BAD(Raise);
	BAD(Sel);
	BAD(InstOf);
	BAD(Cast);
	BAD(Free);
	BAD(Tuple);
	BAD(Id);
	//BAD(Bad);
	BAD(Confirm);
	BAD(Filter);
	BAD(CallBegin);
	BAD(EndReg);
	BAD(EndExcept);

	/* handle generic backend nodes */
	GEN(be_FrameAddr);
	//GEN(be_Call);
	GEN(be_Return);
	GEN(be_FrameLoad);
	GEN(be_FrameStore);
	GEN(be_StackParam);
	GEN(be_AddSP);
	GEN(be_SubSP);
	GEN(be_Copy);

	/* set the register for all Unknown nodes */
	GEN(Unknown);

	op_Mulh = get_op_Mulh();
	if (op_Mulh)
		GEN(Mulh);

#undef GEN
#undef BAD
}

/**
 * Pre-transform all unknown and noreg nodes.
 */
static void ia32_pretransform_node(void *arch_cg) {
	ia32_code_gen_t *cg = arch_cg;

	cg->unknown_gp  = be_pre_transform_node(cg->unknown_gp);
	cg->unknown_vfp = be_pre_transform_node(cg->unknown_vfp);
	cg->unknown_xmm = be_pre_transform_node(cg->unknown_xmm);
	cg->noreg_gp    = be_pre_transform_node(cg->noreg_gp);
	cg->noreg_vfp   = be_pre_transform_node(cg->noreg_vfp);
	cg->noreg_xmm   = be_pre_transform_node(cg->noreg_xmm);
}

static
void add_missing_keep_walker(ir_node *node, void *data)
{
	int              n_outs, i;
	unsigned         found_projs = 0;
	const ir_edge_t *edge;
	ir_mode         *mode = get_irn_mode(node);
	ir_node         *last_keep;
	(void) data;
	if(mode != mode_T)
		return;
	if(!is_ia32_irn(node))
		return;

	n_outs = get_ia32_n_res(node);
	if(n_outs <= 0)
		return;
	if(is_ia32_SwitchJmp(node))
		return;

	assert(n_outs < (int) sizeof(unsigned) * 8);
	foreach_out_edge(node, edge) {
		ir_node *proj = get_edge_src_irn(edge);
		int      pn   = get_Proj_proj(proj);

		assert(pn < n_outs);
		found_projs |= 1 << pn;
	}


	/* are keeps missing? */
	last_keep = NULL;
	for(i = 0; i < n_outs; ++i) {
		ir_node                     *block;
		ir_node                     *in[1];
		const arch_register_req_t   *req;
		const arch_register_class_t *class;

		if(found_projs & (1 << i)) {
			continue;
		}

		req   = get_ia32_out_req(node, i);
		class = req->cls;
		if(class == NULL) {
			continue;
		}

		block = get_nodes_block(node);
		in[0] = new_r_Proj(current_ir_graph, block, node,
		                   arch_register_class_mode(class), i);
		if(last_keep != NULL) {
			be_Keep_add_node(last_keep, class, in[0]);
		} else {
			last_keep = be_new_Keep(class, current_ir_graph, block, 1, in);
		}
	}
}

/**
 * Adds missing keeps to nodes
 */
static
void add_missing_keeps(ia32_code_gen_t *cg)
{
	ir_graph *irg = be_get_birg_irg(cg->birg);
	irg_walk_graph(irg, add_missing_keep_walker, NULL, NULL);
}

/* do the transformation */
void ia32_transform_graph(ia32_code_gen_t *cg) {
	register_transformers();
	env_cg = cg;
	be_transform_graph(cg->birg, ia32_pretransform_node, cg);
	edges_verify(cg->irg);
	add_missing_keeps(cg);
	edges_verify(cg->irg);
}

void ia32_init_transform(void)
{
	FIRM_DBG_REGISTER(dbg, "firm.be.ia32.transform");
}
