/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief       Block-scheduling strategies.
 * @author      Matthias Braun, Christoph Mallon
 * @date        27.09.2006
 *
 * The goals of the greedy (and ILP) algorithm here works by assuming that
 * we want to change as many jumps to fallthroughs as possible (executed jumps
 * actually, we have to look at the execution frequencies). The algorithms
 * do this by collecting execution frequencies of all branches (which is easily
 * possible when all critical edges are split) then removes critical edges where
 * possible as we don't need and want them anymore now. The algorithms then try
 * to change as many edges to fallthroughs as possible, this is done by setting
 * a next and prev pointers on blocks. The greedy algorithm sorts the edges by
 * execution frequencies and tries to transform them to fallthroughs in this order
 */
#include "beblocksched.h"

#include <stdlib.h>

#include "util.h"
#include "array.h"
#include "pdeq.h"
#include "beirg.h"
#include "iredges.h"
#include "irgwalk.h"
#include "irnode_t.h"
#include "irgraph_t.h"
#include "irgmod.h"
#include "irloop.h"
#include "execfreq.h"
#include "irdump_t.h"
#include "irtools.h"
#include "debug.h"
#include "bearch.h"
#include "bemodule.h"
#include "besched.h"
#include "beutil.h"
#include "be.h"
#include "panic.h"

DEBUG_ONLY(static firm_dbg_module_t *dbg = NULL;)

static bool blocks_removed;

/**
 * Post-block-walker: Find blocks containing only one jump and
 * remove them.
 */
static void remove_empty_block(ir_node *block)
{
	if (irn_visited_else_mark(block))
		return;

	if (get_Block_n_cfgpreds(block) != 1)
		goto check_preds;

	ir_node *jump = NULL;
	sched_foreach(block, node) {
		if (!is_Jmp(node)
		    && !(arch_get_irn_flags(node) & arch_irn_flag_simple_jump))
			goto check_preds;
		if (jump != NULL) {
			/* we should never have 2 jumps in a block */
			panic("found 2 jumps in a block");
		}
		jump = node;
	}
	if (jump == NULL)
		goto check_preds;

	ir_entity *entity     = get_Block_entity(block);
	ir_node   *pred       = get_Block_cfgpred(block, 0);
	ir_node   *succ_block = NULL;
	foreach_out_edge_safe(jump, edge) {
		int pos = get_edge_src_pos(edge);

		assert(succ_block == NULL);
		succ_block = get_edge_src_irn(edge);
		if (get_Block_entity(succ_block) != NULL && entity != NULL) {
			/* Currently we can add only one label for a block. Therefore we
			 * cannot combine them if both block already have one. :-( */
			goto check_preds;
		}

		set_irn_n(succ_block, pos, pred);
	}

	/* move the label to the successor block */
	set_Block_entity(succ_block, entity);

	/* there can be some non-scheduled Pin nodes left in the block, move them
	 * to the succ block (Pin) or pred block (Sync) */
	foreach_out_edge_safe(block, edge) {
		ir_node *const node = get_edge_src_irn(edge);

		if (node == jump)
			continue;
		/* we simply kill Pins, because there are some strange interactions
		 * between jump threading, which produce PhiMs with Pins, we simply
		 * kill the pins here, everything is scheduled anyway */
		if (is_Pin(node)) {
			exchange(node, get_Pin_op(node));
			continue;
		}
		if (is_Sync(node)) {
			set_nodes_block(node, get_nodes_block(pred));
			continue;
		}
		if (is_End(node)) { /* End-keep, reroute it to the successor */
			int pos = get_edge_src_pos(edge);
			set_irn_n(node, pos, succ_block);
			continue;
		}
		panic("unexpected node %+F in block %+F with empty schedule", node, block);
	}

	ir_graph *irg = get_irn_irg(block);
	set_Block_cfgpred(block, 0, new_r_Bad(irg, mode_X));
	kill_node(jump);
	blocks_removed = true;

	/* check predecessor */
	remove_empty_block(get_nodes_block(pred));
	return;

check_preds:
	for (int i = 0, arity = get_Block_n_cfgpreds(block); i < arity; ++i) {
		ir_node *pred = get_Block_cfgpred_block(block, i);
		remove_empty_block(pred);
	}
}

/* removes basic blocks that just contain a jump instruction */
static void remove_empty_blocks(ir_graph *irg)
{
	blocks_removed = false;

	ir_reserve_resources(irg, IR_RESOURCE_IRN_VISITED);
	inc_irg_visited(irg);
	remove_empty_block(get_irg_end_block(irg));
	foreach_irn_in(get_irg_end(irg), i, pred) {
		if (!is_Block(pred))
			continue;
		remove_empty_block(pred);
	}
	ir_free_resources(irg, IR_RESOURCE_IRN_VISITED);

	if (blocks_removed) {
		/* invalidate analysis info */
		clear_irg_properties(irg, IR_GRAPH_PROPERTY_CONSISTENT_DOMINANCE);
	}
}

typedef struct blocksched_entry_t blocksched_entry_t;
struct blocksched_entry_t {
	ir_node            *block;
	blocksched_entry_t *next;
	blocksched_entry_t *prev;
};

typedef struct edge_t edge_t;
struct edge_t {
	ir_node *block;             /**< source block */
	int     pos;                /**< number of cfg predecessor (target) */
	double  execfreq;           /**< the frequency */
	double  outedge_penalty_freq; /**< for edges leaving the loop this is the
	                                   penality when we make them a
	                                   fallthrough. */
	int     highest_execfreq;   /**< flag that indicates whether this edge is
	                                 the edge with the highest execfreq pointing
	                                 away from this block */
};

typedef struct blocksched_env_t blocksched_env_t;
struct blocksched_env_t {
	ir_graph       *irg;
	struct obstack  obst;
	edge_t         *edges;
	pdeq           *worklist;
	int            blockcount;
};

static blocksched_entry_t* get_blocksched_entry(const ir_node *block)
{
	return (blocksched_entry_t*)get_irn_link(block);
}

/**
 * Collect cfg frequencies of all edges between blocks.
 * Also determines edge with highest frequency.
 */
static void collect_egde_frequency(ir_node *block, void *data)
{
	blocksched_env_t   *env = (blocksched_env_t*)data;
	int                arity;
	edge_t             edge;
	blocksched_entry_t *entry;
	ir_loop            *loop;

	memset(&edge, 0, sizeof(edge));

	entry = OALLOCZ(&env->obst, blocksched_entry_t);
	entry->block = block;
	set_irn_link(block, entry);

	loop = get_irn_loop(block);

	arity = get_Block_n_cfgpreds(block);

	if (arity == 0) {
		/* must be the start block (or end-block for endless loops),
		 * everything else is dead code and should be removed by now */
		assert(block == get_irg_start_block(env->irg)
				|| block == get_irg_end_block(env->irg));
		/* nothing to do here */
		return;
	} else if (arity == 1) {
		ir_node *pred_block = get_Block_cfgpred_block(block, 0);
		ir_loop *pred_loop  = get_irn_loop(pred_block);
		float    freq       = (float)get_block_execfreq(block);

		/* is it an edge leaving a loop */
		if (get_loop_depth(pred_loop) > get_loop_depth(loop)) {
			float pred_freq = (float)get_block_execfreq(pred_block);
			edge.outedge_penalty_freq = -(pred_freq - freq);
		}

		edge.block            = block;
		edge.pos              = 0;
		edge.execfreq         = freq;
		edge.highest_execfreq = 1;
		ARR_APP1(edge_t, env->edges, edge);
	} else {
		int    i;
		double highest_execfreq = -1.0;
		int    highest_edge_num = -1;

		edge.block = block;
		for (i = 0; i < arity; ++i) {
			double  execfreq;
			ir_node *pred_block = get_Block_cfgpred_block(block, i);

			execfreq = get_block_execfreq(pred_block);

			edge.pos              = i;
			edge.execfreq         = execfreq;
			edge.highest_execfreq = 0;
			ARR_APP1(edge_t, env->edges, edge);

			if (execfreq > highest_execfreq) {
				highest_execfreq = execfreq;
				highest_edge_num = ARR_LEN(env->edges) - 1;
			}
		}

		if (highest_edge_num >= 0)
			env->edges[highest_edge_num].highest_execfreq = 1;
	}
}

static int cmp_edges_base(const edge_t *e1, const edge_t *e2)
{
	long nr1 = get_irn_node_nr(e1->block);
	long nr2 = get_irn_node_nr(e2->block);
	if (nr1 < nr2) {
		return 1;
	} else if (nr1 > nr2) {
		return -1;
	} else {
		if (e1->pos < e2->pos) {
			return 1;
		} else if (e1->pos > e2->pos) {
			return -1;
		} else {
			return 0;
		}
	}
}

static int cmp_edges(const void *d1, const void *d2)
{
	const edge_t *e1 = (const edge_t*)d1;
	const edge_t *e2 = (const edge_t*)d2;
	double        freq1 = e1->execfreq;
	double        freq2 = e2->execfreq;
	if (freq1 < freq2) {
		return 1;
	} else if (freq1 > freq2) {
		return -1;
	} else {
		return cmp_edges_base(e1, e2);
	}
}

static int cmp_edges_outedge_penalty(const void *d1, const void *d2)
{
	const edge_t *e1   = (const edge_t*)d1;
	const edge_t *e2   = (const edge_t*)d2;
	double        pen1 = e1->outedge_penalty_freq;
	double        pen2 = e2->outedge_penalty_freq;
	if (pen1 > pen2) {
		return 1;
	} else if (pen1 < pen2) {
		return -1;
	} else {
		return cmp_edges_base(e1, e2);
	}
}

static void clear_loop_links(ir_loop *loop)
{
	int i, n;

	set_loop_link(loop, NULL);
	n = get_loop_n_elements(loop);
	for (i = 0; i < n; ++i) {
		loop_element elem = get_loop_element(loop, i);
		if (*elem.kind == k_ir_loop) {
			clear_loop_links(elem.son);
		}
	}
}

static void coalesce_blocks(blocksched_env_t *env)
{
	int i;
	int edge_count = ARR_LEN(env->edges);
	edge_t *edges = env->edges;

	/* sort interblock edges by execution frequency */
	QSORT_ARR(edges, cmp_edges);

	/* run1: only look at jumps */
	for (i = 0; i < edge_count; ++i) {
		const edge_t *edge  = &edges[i];
		ir_node      *block = edge->block;
		int           pos   = edge->pos;
		ir_node      *pred_block;
		blocksched_entry_t *entry, *pred_entry;

		/* only check edge with highest frequency */
		if (! edge->highest_execfreq)
			continue;

		/* the block might have been removed already... */
		if (is_Bad(get_Block_cfgpred(block, 0)))
			continue;

		pred_block = get_Block_cfgpred_block(block, pos);
		entry      = get_blocksched_entry(block);
		pred_entry = get_blocksched_entry(pred_block);

		if (pred_entry->next != NULL || entry->prev != NULL)
			continue;

		/* only coalesce jumps */
		if (get_block_succ_next(pred_block, get_block_succ_first(pred_block)) != NULL)
			continue;

		/* schedule the 2 blocks behind each other */
		DB((dbg, LEVEL_1, "Coalesce (Jump) %+F -> %+F (%.3g)\n",
		           pred_entry->block, entry->block, edge->execfreq));
		pred_entry->next = entry;
		entry->prev      = pred_entry;
	}

	/* run2: pick loop fallthroughs */
	clear_loop_links(get_irg_loop(env->irg));

	QSORT_ARR(edges, cmp_edges_outedge_penalty);
	for (i = 0; i < edge_count; ++i) {
		const edge_t *edge  = &edges[i];
		ir_node      *block = edge->block;
		int           pos   = edge->pos;
		ir_node      *pred_block;
		blocksched_entry_t *entry, *pred_entry;
		ir_loop      *loop;
		ir_loop      *outer_loop;

		/* already seen all loop outedges? */
		if (edge->outedge_penalty_freq == 0)
			break;

		/* the block might have been removed already... */
		if (is_Bad(get_Block_cfgpred(block, pos)))
			continue;

		pred_block = get_Block_cfgpred_block(block, pos);
		entry      = get_blocksched_entry(block);
		pred_entry = get_blocksched_entry(pred_block);

		if (pred_entry->next != NULL || entry->prev != NULL)
			continue;

		/* we want at most 1 outedge fallthrough per loop */
		loop = get_irn_loop(pred_block);
		if (get_loop_link(loop) != NULL)
			continue;

		/* schedule the 2 blocks behind each other */
		DB((dbg, LEVEL_1, "Coalesce (Loop Outedge) %+F -> %+F (%.3g)\n",
		           pred_entry->block, entry->block, edge->execfreq));
		pred_entry->next = entry;
		entry->prev      = pred_entry;

		/* all loops left have an outedge now */
		outer_loop = get_irn_loop(block);
		do {
			/* we set loop link to loop to mark it */
			set_loop_link(loop, loop);
			loop = get_loop_outer_loop(loop);
		} while (loop != outer_loop);
	}

	/* sort interblock edges by execution frequency */
	QSORT_ARR(edges, cmp_edges);

	/* run3: remaining edges */
	for (i = 0; i < edge_count; ++i) {
		const edge_t *edge  = &edges[i];
		ir_node      *block = edge->block;
		int           pos   = edge->pos;
		ir_node      *pred_block;
		blocksched_entry_t *entry, *pred_entry;

		/* the block might have been removed already... */
		if (is_Bad(get_Block_cfgpred(block, pos)))
			continue;

		pred_block = get_Block_cfgpred_block(block, pos);
		entry      = get_blocksched_entry(block);
		pred_entry = get_blocksched_entry(pred_block);

		/* is 1 of the blocks already attached to another block? */
		if (pred_entry->next != NULL || entry->prev != NULL)
			continue;

		/* schedule the 2 blocks behind each other */
		DB((dbg, LEVEL_1, "Coalesce (CondJump) %+F -> %+F (%.3g)\n",
		           pred_entry->block, entry->block, edge->execfreq));
		pred_entry->next = entry;
		entry->prev      = pred_entry;
	}
}

static void pick_block_successor(blocksched_entry_t *entry, blocksched_env_t *env)
{
	ir_node            *block = entry->block;
	ir_node            *succ  = NULL;
	blocksched_entry_t *succ_entry;
	double              best_succ_execfreq;

	if (irn_visited_else_mark(block))
		return;

	env->blockcount++;

	DB((dbg, LEVEL_1, "Pick succ of %+F\n", block));

	/* put all successors into the worklist */
	foreach_block_succ(block, edge) {
		ir_node *succ_block = get_edge_src_irn(edge);

		if (irn_visited(succ_block))
			continue;

		/* we only need to put the first of a series of already connected
		 * blocks into the worklist */
		succ_entry = get_blocksched_entry(succ_block);
		while (succ_entry->prev != NULL) {
			/* break cycles... */
			if (succ_entry->prev->block == succ_block) {
				succ_entry->prev->next = NULL;
				succ_entry->prev       = NULL;
				break;
			}
			succ_entry = succ_entry->prev;
		}

		if (irn_visited(succ_entry->block))
			continue;

		DB((dbg, LEVEL_1, "Put %+F into worklist\n", succ_entry->block));
		pdeq_putr(env->worklist, succ_entry->block);
	}

	if (entry->next != NULL) {
		pick_block_successor(entry->next, env);
		return;
	}

	DB((dbg, LEVEL_1, "deciding...\n"));
	best_succ_execfreq = -1;

	/* no successor yet: pick the successor block with the highest execution
	 * frequency which has no predecessor yet */

	foreach_block_succ(block, edge) {
		ir_node *succ_block = get_edge_src_irn(edge);

		if (irn_visited(succ_block))
			continue;

		succ_entry = get_blocksched_entry(succ_block);
		if (succ_entry->prev != NULL)
			continue;

		double execfreq = get_block_execfreq(succ_block);
		if (execfreq > best_succ_execfreq) {
			best_succ_execfreq = execfreq;
			succ = succ_block;
		}
	}

	if (succ == NULL) {
		DB((dbg, LEVEL_1, "pick from worklist\n"));

		do {
			if (pdeq_empty(env->worklist)) {
				DB((dbg, LEVEL_1, "worklist empty\n"));
				return;
			}
			succ = (ir_node*)pdeq_getl(env->worklist);
		} while (irn_visited(succ));
	}

	succ_entry       = get_blocksched_entry(succ);
	entry->next      = succ_entry;
	succ_entry->prev = entry;

	pick_block_successor(succ_entry, env);
}

static blocksched_entry_t *finish_block_schedule(blocksched_env_t *env)
{
	ir_graph           *irg        = env->irg;
	ir_node            *startblock = get_irg_start_block(irg);
	blocksched_entry_t *entry      = get_blocksched_entry(startblock);

	ir_reserve_resources(irg, IR_RESOURCE_IRN_VISITED);
	inc_irg_visited(irg);

	env->worklist = new_pdeq();
	pick_block_successor(entry, env);
	assert(pdeq_empty(env->worklist));
	del_pdeq(env->worklist);

	ir_free_resources(irg, IR_RESOURCE_IRN_VISITED);

	return entry;
}

static ir_node **create_blocksched_array(blocksched_env_t *env,
                                         blocksched_entry_t *first,
                                         int count, struct obstack* obst)
{
	int                i = 0;
	ir_node            **block_list;
	blocksched_entry_t *entry;
	(void) env;

	block_list = NEW_ARR_D(ir_node *, obst, count);
	DB((dbg, LEVEL_1, "Blockschedule:\n"));

	for (entry = first; entry != NULL; entry = entry->next) {
		assert(i < count);
		block_list[i++] = entry->block;
		DB((dbg, LEVEL_1, "\t%+F\n", entry->block));
	}
	assert(i == count);

	return block_list;
}

ir_node **be_create_block_schedule(ir_graph *irg)
{
	blocksched_env_t   env;
	blocksched_entry_t *start_entry;
	ir_node            **block_list;

	env.irg        = irg;
	env.edges      = NEW_ARR_F(edge_t, 0);
	env.worklist   = NULL;
	env.blockcount = 0;
	obstack_init(&env.obst);

	assure_loopinfo(irg);

	// collect edge execution frequencies
	irg_block_walk_graph(irg, collect_egde_frequency, NULL, &env);

	remove_empty_blocks(irg);

	coalesce_blocks(&env);

	start_entry = finish_block_schedule(&env);
	block_list  = create_blocksched_array(&env, start_entry, env.blockcount,
	                                      be_get_be_obst(irg));

	DEL_ARR_F(env.edges);
	obstack_free(&env.obst, NULL);

	return block_list;
}

BE_REGISTER_MODULE_CONSTRUCTOR(be_init_blocksched)
void be_init_blocksched(void)
{
	FIRM_DBG_REGISTER(dbg, "firm.be.blocksched");
}
