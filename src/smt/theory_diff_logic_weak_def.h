/*++
Copyright (c) 2006 Microsoft Corporation

Module Name:

    theory_diff_logic_weak_def.h

Abstract:

    Weakened Difference Logic

Author:

    Leonardo de Moura (leonardo) 2006-11-29.
    Nikolaj Bjorner (nbjorner) 2008-05-11

Revision History:

    2008-05-11 ported from v1.2. Add theory propagation.

--*/
#pragma once

#include "util/map.h"
#include "util/warning.h"
#include "ast/ast_pp.h"
#include "smt/theory_diff_logic_weak.h"
#include "smt/smt_context.h"
#include "smt/smt_model_generator.h"
#include "model/model_implicant.h"


using namespace smt;


template<typename Ext>
theory_diff_logic_weak<Ext>::theory_diff_logic_weak(context& ctx):
    theory(ctx, ctx.get_manager().mk_family_id("arith")),
    m_params(ctx.get_fparams()),
    m_util(ctx.get_manager()),
    m_arith_eq_adapter(*this, m_util),
    m_consistent(true),
    m_izero(null_theory_var),
    m_rzero(null_theory_var),
    m_terms(ctx.get_manager()),
    m_asserted_qhead(0),
    m_equation_qhead(0),
    m_num_core_conflicts(0),
    m_num_propagation_calls(0),
    m_agility(0.5),
    m_lia_or_lra(not_set),
    m_non_diff_logic_exprs(false),
    m_factory(nullptr),
    m_nc_functor(*this),
    m_S(ctx.get_manager().limit()),
    m_num_simplex_edges(0),
    m_var_value_table(DEFAULT_HASHTABLE_INITIAL_CAPACITY, var_value_hash(*this), var_value_eq(*this)) {
}            

template<typename Ext>
std::ostream& theory_diff_logic_weak<Ext>::atom::display(theory_diff_logic_weak const& th, std::ostream& out) const { 
    context& ctx = th.get_context();
    lbool asgn = ctx.get_assignment(m_bvar);
    //SASSERT(asgn == l_undef || ((asgn == l_true) == m_true));
    bool sign = (l_undef == asgn) || m_true;
    return out << literal(m_bvar, sign) 
               << " " << mk_pp(ctx.bool_var2expr(m_bvar), th.get_manager()) << " "; 
    if (l_undef == asgn) {
        out << "unassigned\n";
    }
    else {
        th.m_graph.display_edge(out, get_asserted_edge());
    }
    return out;
}

// -----------------------------------------
// theory_diff_logic_weak::nc_functor

template<typename Ext>
void theory_diff_logic_weak<Ext>::nc_functor::reset() {
    m_antecedents.reset();
}


// -----------------------------------------
// theory_diff_logic_weak


template<typename Ext>
bool theory_diff_logic_weak<Ext>::internalize_term(app * term) {
    IF_VERBOSE(15, verbose_stream() << "W-DL: internalize_term: " << mk_pp(term, m) << "\n";);
    if (!m_consistent)
        return false;
    bool result = null_theory_var != mk_term(term);
    CTRACE("arith", !result, tout << "Did not internalize " << mk_pp(term, m) << "\n";);
    if (!result) {
        TRACE("non_diff_logic", tout << "Terms may not be internalized\n";);
        found_non_diff_logic_expr(term);
    }
    return result;
}

template<typename numeral>
class diff_logic_bounds {
    bool m_inf_is_set;
    bool m_sup_is_set;
    bool m_eq_found;
    literal m_inf_l;
    literal m_sup_l;
    literal m_eq_l;
    numeral m_inf_w;
    numeral m_sup_w;
    numeral m_w;
    
public:
    diff_logic_bounds() {
        reset(numeral(0));
    }
    void reset(numeral const& w) {
        m_inf_is_set = false;
        m_sup_is_set = false;
        m_eq_found = false;
        m_inf_l = null_literal;
        m_sup_l = null_literal;
        m_eq_l = null_literal;
        m_w = w;
    }

    void operator()(numeral const& w, literal l) {
        if (l != null_literal) {
            if ((w < m_w) && (!m_inf_is_set || w > m_inf_w)) {
                m_inf_w = w;
                m_inf_l = l;
                m_inf_is_set = true;
            }
            else if ((w > m_w) && (!m_sup_is_set || w < m_sup_w)) {
                m_sup_w = w;
                m_sup_l = l;
                m_sup_is_set = true;
            }
            else if (w == m_w) {
                m_eq_found = true;
                m_eq_l = l;
            }
        }
    }

    bool get_inf(numeral& w, literal& l) const {
        w = m_inf_w;
        l = m_inf_l;
        return m_inf_is_set;
    }

    bool get_sup(numeral& w, literal& l) const {
        w = m_sup_w;
        l = m_sup_l;
        return m_sup_is_set;
    }
    
    bool get_eq(literal& l) const {
        l = m_eq_l;
        return m_eq_found;
    }
    
};

// 
// Atoms are of the form x +  -1*y <= k, or x + -1*y = k
//

template<typename Ext>
void theory_diff_logic_weak<Ext>::found_non_diff_logic_expr(expr * n) {
    if (!m_non_diff_logic_exprs) {
        TRACE("non_diff_logic", tout << "found non diff logic expression:\n" << mk_pp(n, m) << "\n";);
        IF_VERBOSE(0, verbose_stream() << "(smt.diff_logic: non-diff logic expression " << mk_pp(n, m) << ")\n";); 
        ctx.push_trail(value_trail<context, bool>(m_non_diff_logic_exprs));
        m_non_diff_logic_exprs = true;
    }
}

template<typename Ext>
bool theory_diff_logic_weak<Ext>::internalize_atom(app * n, bool gate_ctx) {
    IF_VERBOSE(15, verbose_stream() << "W-DL: internalize_atom with gate_ctx(" << gate_ctx << "): " << mk_pp(n, m) << "\n";);
    if (!m_consistent)
        return false;
    if (!m_util.is_le(n) && !m_util.is_ge(n)) {
        found_non_diff_logic_expr(n);
        return false;
    }
    SASSERT(m_util.is_le(n) || m_util.is_ge(n));
    SASSERT(!ctx.b_internalized(n));

    bool is_ge = m_util.is_ge(n);
    bool_var bv;
    rational kr;
    theory_var source, target; // target - source <= k
    app * lhs = to_app(n->get_arg(0));
    IF_VERBOSE(15, verbose_stream() << "W-DL: internalize_atom: LHS = " << mk_pp(lhs, m) << "\n";);
    app * rhs = to_app(n->get_arg(1));
    IF_VERBOSE(15, verbose_stream() << "W-DL: internalize_atom: RHS = " << mk_pp(rhs, m) << "\n";);
    if (!m_util.is_numeral(rhs)) {
        std::swap(rhs, lhs);
        is_ge = !is_ge;
    }
    if (!m_util.is_numeral(rhs, kr)) {
        found_non_diff_logic_expr(n);
        return false;
    }
    numeral k(kr);

    m_terms.reset();
    m_signs.reset();
    m_terms.push_back(lhs);
    m_signs.push_back(true);
    if (!decompose_linear(m_terms, m_signs)) {
        found_non_diff_logic_expr(n);        
        return false;
    }
    SASSERT(m_signs.size() == m_terms.size());
    if (m_terms.size() == 2 && m_signs[0] != m_signs[1]) {
        app* a = m_terms.get(0), *b = m_terms.get(1);
        bool sign0 = m_signs[0];
        target = mk_var(a);
        source = mk_var(b);
        if (!sign0) {
            std::swap(target, source);
        }
    }
    else {
        target = mk_var(lhs);
        source = get_zero(m_util.is_int(lhs));
    }

    if (is_ge) {
        std::swap(target, source);
        k.neg();
    }

    if (ctx.b_internalized(n)) return true;
    bv = ctx.mk_bool_var(n);
    ctx.set_var_theory(bv, get_id());
    literal l(bv);

    // get_assignment(bv);

    // 
    // Create axioms for situations as:
    // x - y <= 5 => x - y <= 7
    //

    if (m_params.m_arith_add_binary_bounds) {
        literal l0;
        numeral k0;
        diff_logic_bounds<numeral> bounds;
        bounds.reset(k);
        m_graph.enumerate_edges(source, target, bounds);
        if (bounds.get_eq(l0)) {
            ctx.mk_th_axiom(get_id(),~l0,l);
            ctx.mk_th_axiom(get_id(),~l,l0);
        }
        else {
            if (bounds.get_inf(k0, l0)) {
                SASSERT(k0 <= k);
                ctx.mk_th_axiom(get_id(),~l0,l);
            }
            if (bounds.get_sup(k0, l0)) {
                SASSERT(k <= k0);
                ctx.mk_th_axiom(get_id(),~l,l0);
            }
        }
    }
    SASSERT(m_util.is_numeral(rhs));

    IF_VERBOSE(5, verbose_stream() << "W-DL: expr:\n" << mk_pp(n, m) << "\n");
    IF_VERBOSE(5, verbose_stream() << "W-DL: edge: src_id #" << source << ", dst_id #" << target << ", weight: " << k << ", gate_ctx(" << gate_ctx << ")\n";);

    edge_id pos = m_graph.add_edge(source, target,  k, l);
    k.neg();
    if (m_util.is_int(lhs)) {
        SASSERT(k.is_int());
        k -= numeral(1);
    }
    else {
        k -= this->m_epsilon; 
    }
    edge_id neg = m_graph.add_edge(target, source, k, ~l);
    atom * a = alloc(atom, bv, pos, neg);
    m_atoms.push_back(a);
    m_bool_var2atom.insert(bv, a);
    IF_VERBOSE(15, verbose_stream() << "W-DL: internalize_atom done:\nexpr:\n" << mk_pp(n, m) << "\nedge:\n";
        a->display(*this, verbose_stream());
        verbose_stream() << "\n";
        m_graph.display_edge(verbose_stream() << "\tpos #"<< pos << ": ", pos);
        m_graph.display_edge(verbose_stream() << "\tneg #"<< neg << ": ", neg); );
    IF_VERBOSE(15, verbose_stream() << "\nW-DL: dl-graph display:\n";
        display(verbose_stream()); );
    TRACE("arith", 
        tout << mk_pp(n, m) << "\n";
        m_graph.display_edge(tout << "pos: ", pos); 
          m_graph.display_edge(tout << "neg: ", neg); 
        );
    return true;

    // keeps x <= k and its weight
    // keeps x - y <= 0
    // if (target == 0 || source == 0) {
    //     IF_VERBOSE(5, verbose_stream() << "W-DL: edge with single variable (edge to 0)\n";);
    //     numeral weight_pos(k);
    //     k.neg();
    //     if (m_util.is_int(lhs)) {
    //         SASSERT(k.is_int());
    //         k -= numeral(1);
    //     }
    //     else {
    //         k -= this->m_epsilon; 
    //     }
    //     edge_id pos = m_graph.add_edge(source, target, weight_pos, l);
    //     edge_id neg = m_graph.add_edge(target, source, weight_neg, ~l);
    //     atom * a = alloc(atom, bv, pos, neg);
    //     m_atoms.push_back(a);
    //     m_bool_var2atom.insert(bv, a);

    //     IF_VERBOSE(15, verbose_stream() << "W-DL: internalize_atom done:\nexpr:\n" << mk_pp(n, m) << "\nedge:\n";
    //         a->display(*this, verbose_stream());
    //         verbose_stream() << "\n";
    //         m_graph.display_edge(verbose_stream() << "\tpos #"<< pos << ": ", pos);
    //         m_graph.display_edge(verbose_stream() << "\tneg #"<< neg << ": ", neg); );
    //     IF_VERBOSE(15, verbose_stream() << "\nW-DL: dl-graph display:\n";
    //         display(verbose_stream()); );
    //     IF_VERBOSE(5, verbose_stream() << "\nW-DL: equation list display:\nkeep: " << m_equation_kept << "\nelim: " << m_equation_elim << "\nweig: "; display_equws(verbose_stream(), m_equation_weight););
    //     return true;
    // }

    // // check equal first
    // if (!m_atoms.empty()) {
    //     IF_VERBOSE(5, verbose_stream() << "W-DL: check EQUAL\n";);
    //     // check last atom
    //     atom * prev_a = m_atoms.back();
    //     bool_var prev_bv = prev_a->get_bool_var();
    //     literal prev_l(prev_bv);
    //     expr * prev_n = ctx.bool_var2expr(prev_bv);
    //     app * prev_lhs = to_app(to_app(prev_n)->get_arg(0));
    //     app * prev_rhs = to_app(to_app(prev_n)->get_arg(1));
    //     IF_VERBOSE(15, verbose_stream() << "W-DL: previous expr = " << mk_pp(prev_n, m) << "\nLHS = " << mk_pp(prev_lhs, m) << ", RHS = " << mk_pp(prev_rhs, m) << "\n";);

    //     // keeps x - y == k: both edges
    //     if ((prev_lhs == lhs) && (prev_rhs == rhs)) {
    //         IF_VERBOSE(5, verbose_stream() << "W-DL: EQUAL\n";);
            
    //         numeral k2(k);
    //         k.neg();
    //         numeral k1(k);

    //         theory_var src; theory_var dst; numeral wgt;
    //         if (k2 > numeral(0)) { // k > 0
    //             int source_idx = m_equation_elim.index(source);
    //             int target_idx = m_equation_elim.index(target);
    //             if ((source_idx != -1) && (target_idx == -1)) { // if exists
    //                 theory_var kept_old = m_equation_kept[source_idx];
    //                 numeral weight_old = m_equation_weight[source_idx];
    //                 m_equation_kept.push_back(kept_old);
    //                 m_equation_elim.push_back(target);
    //                 m_equation_weight.push_back(weight_old + k2);
    //                 src = kept_old; dst = target; wgt = weight_old + k2;
    //             }
    //             else if ((source_idx == -1) && (target_idx != -1)) {
    //                 theory_var kept_old = m_equation_kept[target_idx];
    //                 numeral weight_old = m_equation_weight[target_idx];
    //                 m_equation_kept.push_back(kept_old);
    //                 m_equation_elim.push_back(source);
    //                 m_equation_weight.push_back(weight_old - k2);
    //                 src = kept_old; dst = source; wgt = weight_old - k2;
    //             }
    //             else if ((source_idx == -1) && (target_idx == -1)) { // target = source + (>0)
    //                 m_equation_kept.push_back(source);
    //                 m_equation_elim.push_back(target);
    //                 m_equation_weight.push_back(k2);
    //                 src = source; dst = target; wgt = k2;
    //             }
    //             else {
    //                 src = source; dst = target; wgt = k2;
    //             }
    //         }
    //         else { // k <= 0
    //             int source_idx = m_equation_elim.index(source);
    //             int target_idx = m_equation_elim.index(target);
    //             if ((source_idx == -1) && (target_idx != -1)) { // if exists
    //                 theory_var kept_old = m_equation_kept[target_idx];
    //                 numeral weight_old = m_equation_weight[target_idx];
    //                 m_equation_kept.push_back(kept_old);
    //                 m_equation_elim.push_back(source);
    //                 m_equation_weight.push_back(weight_old + k1);
    //                 src = kept_old; dst = source; wgt = weight_old + k1;
    //             }
    //             else if ((source_idx != -1) && (target_idx == -1)) {
    //                 theory_var kept_old = m_equation_kept[source_idx];
    //                 numeral weight_old = m_equation_weight[source_idx];
    //                 m_equation_kept.push_back(kept_old);
    //                 m_equation_elim.push_back(target); 
    //                 m_equation_weight.push_back(weight_old - k1);
    //                 src = kept_old; dst = target; wgt = weight_old - k1;
    //             }
    //             else if ((source_idx == -1) && (target_idx == -1)) { // source = target + (>0)
    //                 m_equation_kept.push_back(target);
    //                 m_equation_elim.push_back(source); 
    //                 m_equation_weight.push_back(k1);
    //                 src = source; dst = target; wgt = k2;
    //             }
    //             else {
    //                 src = source; dst = target; wgt = k2;
    //             }
    //         }
    //         numeral wgt2(wgt);
    //         wgt.neg();
    //         numeral wgt1(wgt);
    //         edge_id pos1 = m_graph.add_edge(dst, src, wgt1, prev_l);
    //         wgt1.neg();
    //         if (m_util.is_int(lhs)) {
    //             SASSERT(wgt1.is_int());
    //             wgt1 -= numeral(1);
    //         }
    //         else {
    //             wgt1 -= this->m_epsilon;
    //         }
    //         edge_id neg1 = m_graph.add_edge(src, dst, wgt1, ~prev_l);
    //         atom * a1 = alloc(atom, prev_bv, pos1, neg1);
    //         m_atoms.push_back(a1);
    //         m_bool_var2atom.insert(prev_bv, a1);

    //         edge_id pos2 = m_graph.add_edge(src, dst, wgt2, l);
    //         wgt2.neg();
    //         if (m_util.is_int(lhs)) {
    //             SASSERT(wgt2.is_int());
    //             wgt2 -= numeral(1);
    //         }
    //         else {
    //             wgt2 -= this->m_epsilon; 
    //         }
    //         edge_id neg2 = m_graph.add_edge(dst, src, wgt2, ~l);
    //         atom * a2 = alloc(atom, bv, pos2, neg2);
    //         m_atoms.push_back(a2);
    //         m_bool_var2atom.insert(bv, a2);

    //         IF_VERBOSE(15, verbose_stream() << "W-DL: internalize_atom done:\nexpr1:\n" << mk_pp(prev_n, m) << "\nedge:\n";
    //             a1->display(*this, verbose_stream());
    //             verbose_stream() << "\n";
    //             m_graph.display_edge(verbose_stream() << "\tpos #"<< pos1 << ": ", pos1);
    //             m_graph.display_edge(verbose_stream() << "\tneg #"<< neg1 << ": ", neg1); );
    //         IF_VERBOSE(15, verbose_stream() << "W-DL: internalize_atom done:\nexpr2:\n" << mk_pp(n, m) << "\nedge:\n";
    //             a2->display(*this, verbose_stream());
    //             verbose_stream() << "\n";
    //             m_graph.display_edge(verbose_stream() << "\tpos #"<< pos1 << ": ", pos2);
    //             m_graph.display_edge(verbose_stream() << "\tneg #"<< neg2 << ": ", neg2); );
    //         IF_VERBOSE(15, verbose_stream() << "\nW-DL: dl-graph display:\n";
    //             display(verbose_stream()); );
    //         IF_VERBOSE(5, verbose_stream() << "\nW-DL: equation list display:\nkeep: " << m_equation_kept << "\nelim: " << m_equation_elim << "\nweig: "; display_equws(verbose_stream(), m_equation_weight););            
    //         return true;
    //     }
    //     IF_VERBOSE(5, verbose_stream() << "W-DL: not EQUAL\n";);
    // }

    // numeral weight_pos(k);
    // k.neg();
    // if (m_util.is_int(lhs)) {
    //     SASSERT(k.is_int());
    //     k -= numeral(1);
    // }
    // else {
    //     k -= this->m_epsilon; 
    // }
    // numeral weight_neg(k);
    // int source_idx = m_equation_elim.index(source);
    // int target_idx = m_equation_elim.index(target);
    // if ((source_idx != -1) && (target_idx == -1)) {
    //     IF_VERBOSE(5, verbose_stream() << "W-DL: elim src, keep tgt\n");
    //     theory_var kept_old = m_equation_kept[source_idx];
    //     numeral weight_old = m_equation_weight[source_idx];
    //     numeral weight_pos_new = weight_pos + weight_old;
    //     numeral weight_neg_new = weight_neg - weight_old;
    //     edge_id pos; edge_id neg;
    //     if (target == 0 || kept_old == 0 || weight_pos_new == numeral(0))
    //         pos = m_graph.add_edge(kept_old, target, weight_pos_new, l);
    //     else if (weight_pos_new < numeral(0))
    //         pos = m_graph.add_edge(kept_old, target, numeral(-1), l);
    //     else pos = null_edge_id;

    //     if (target == 0 || kept_old == 0 || weight_neg_new == numeral(0))
    //         neg = m_graph.add_edge(target, kept_old, weight_neg_new, ~l);
    //     else if (weight_neg_new <= numeral(0))
    //         neg = m_graph.add_edge(target, kept_old, numeral(-1), ~l);
    //     else neg = null_edge_id;
    //     atom * a = alloc(atom, bv, pos, neg);
    //     m_atoms.push_back(a);
    //     m_bool_var2atom.insert(bv, a);
    // }
    // else if ((source_idx == -1) && (target_idx != -1)) {
    //     IF_VERBOSE(5, verbose_stream() << "W-DL: elim tgt, keep src\n");
    //     theory_var kept_old = m_equation_kept[target_idx];
    //     numeral weight_old = m_equation_weight[target_idx];
    //     numeral weight_pos_new = weight_pos - weight_old;
    //     numeral weight_neg_new = weight_neg + weight_old;
    //     edge_id pos; edge_id neg;
    //     if (source == 0 || kept_old == 0 || weight_pos_new == numeral(0))
    //         pos = m_graph.add_edge(source, kept_old, weight_pos_new, l);
    //     else if (weight_pos_new < numeral(0))
    //         pos = m_graph.add_edge(source, kept_old, numeral(-1), l);
    //     else pos = null_edge_id;

    //     if (source == 0 || kept_old == 0 || weight_neg_new == numeral(0))
    //         neg = m_graph.add_edge(kept_old, source, weight_neg_new, ~l);
    //     else if (weight_neg_new < numeral(0))
    //         neg = m_graph.add_edge(kept_old, source, numeral(-1), ~l);
    //     else neg = null_edge_id;
    //     atom * a = alloc(atom, bv, pos, neg);
    //     m_atoms.push_back(a);
    //     m_bool_var2atom.insert(bv, a);
    // }
    // else if ((source_idx != -1) && (target_idx != -1)) {
    //     IF_VERBOSE(5, verbose_stream() << "W-DL: elim src & tgt\n");
    //     theory_var source_kept_old = m_equation_kept[source_idx];
    //     theory_var target_kept_old = m_equation_kept[target_idx];
    //     numeral source_weight_old = m_equation_weight[source_idx];
    //     numeral target_weight_old = m_equation_weight[target_idx];
    //     numeral weight_pos_new = weight_pos + source_weight_old - target_weight_old;
    //     numeral weight_neg_new = weight_neg - source_weight_old + target_weight_old;
    //     edge_id pos; edge_id neg;
    //     if (weight_pos_new == numeral(0) || source_kept_old == 0 || target_kept_old == 0)
    //         pos = m_graph.add_edge(source_kept_old, target_kept_old, weight_pos_new, l);
    //     else if (weight_pos_new < numeral(0))
    //         pos = m_graph.add_edge(source_kept_old, target_kept_old, numeral(-1), l);
    //     else pos = null_edge_id;

    //     if (weight_neg_new == numeral(0) || source_kept_old == 0 || target_kept_old == 0)
    //         neg = m_graph.add_edge(target_kept_old, source_kept_old, weight_neg_new, ~l);
    //     else if (weight_neg_new < numeral(0))
    //         neg = m_graph.add_edge(target_kept_old, source_kept_old, numeral(-1), ~l);
    //     else neg = null_edge_id;

    //     atom * a = alloc(atom, bv, pos, neg);
    //     m_atoms.push_back(a);
    //     m_bool_var2atom.insert(bv, a);
    // }
    // else {
    //     edge_id pos; edge_id neg;
    //     if (weight_pos == numeral(0) || target == 0 || source == 0) {
    //         IF_VERBOSE(5, verbose_stream() << "W-DL: edge with weight [" << k << "] = 0 or edge to 0\n";);            
    //         pos = m_graph.add_edge(source, target, weight_pos, l);
    //         neg = m_graph.add_edge(target, source, weight_neg, ~l);
    //     }
    //     else if (weight_pos < numeral(0)) {
    //         IF_VERBOSE(5, verbose_stream() << "W-DL: edge with weight [" << k << "] < 0: only positive edge (original weight " << weight_pos << "\n";);            
    //         pos = m_graph.add_edge(source, target, numeral(-1), l);
    //         neg = null_edge_id;
    //     }
    //     else {
    //         IF_VERBOSE(5, verbose_stream() << "W-DL: edge with weight [" << k << "] > 0: only negative edge (original weight " << weight_neg << "\n";);            
    //         pos = null_edge_id;
    //         neg = m_graph.add_edge(target, source, numeral(-1), l);
    //     }
    //     atom * a = alloc(atom, bv, pos, neg);
    //     m_atoms.push_back(a);
    //     m_bool_var2atom.insert(bv, a);
    //     // IF_VERBOSE(5, verbose_stream() << "W-DL: internalize_atom done:\nexpr:\n" << mk_pp(n, m) << "\nedge:\n";
    //     //     a->display(*this, verbose_stream());
    //     //     verbose_stream() << "\n";
    //     //     m_graph.display_edge(verbose_stream() << "\tpos #"<< pos << ": ", pos);
    //     //     m_graph.display_edge(verbose_stream() << "\tneg #"<< neg << ": ", neg); );
    // }
    // IF_VERBOSE(15, verbose_stream() << "\nW-DL: dl-graph display:\n";
    //     display(verbose_stream()); );
    // IF_VERBOSE(5, verbose_stream() << "\nW-DL: equation list display:\nkeep: " << m_equation_kept << "\nelim: " << m_equation_elim << "\nweig: "; display_equws(verbose_stream(), m_equation_weight););
    // return true;
}

template<typename Ext>
void theory_diff_logic_weak<Ext>::internalize_eq_eh(app * atom, bool_var v) {
    IF_VERBOSE(15, verbose_stream() << "W-DL: internalize_eq_eh: " << mk_pp(atom, m) << "\n";);
    TRACE("arith", tout << mk_pp(atom, m) << "\n";);
    app * lhs      = to_app(atom->get_arg(0));
    app * rhs      = to_app(atom->get_arg(1));
    app * s;
    if (m_util.is_add(lhs) && to_app(lhs)->get_num_args() == 2 && 
        is_negative(to_app(to_app(lhs)->get_arg(1)), s) && m_util.is_numeral(rhs)) {
        // force axioms for (= (+ x (* -1 y)) k)
        // this is necessary because (+ x (* -1 y)) is not a diff logic term.
        m_arith_eq_adapter.mk_axioms(ctx.get_enode(lhs), ctx.get_enode(rhs));
        return;
    }
    
    if (m_params.m_arith_eager_eq_axioms) {
        enode * n1 = ctx.get_enode(lhs);
        enode * n2 = ctx.get_enode(rhs);
        if (n1->get_th_var(get_id()) != null_theory_var &&
            n2->get_th_var(get_id()) != null_theory_var)
            m_arith_eq_adapter.mk_axioms(n1, n2);
    }
}

/* // WDL with equation substitution
template<typename Ext>
void theory_diff_logic_weak<Ext>::assign_eh(bool_var v, bool is_true) {
    IF_VERBOSE(5, verbose_stream() << "W-DL: assign_eh: " << v << ": " << is_true << "\n";);
    m_stats.m_num_assertions++;
    atom * a = nullptr;
    // if (m_bool_var2atom.find(v, a)) {
    VERIFY (m_bool_var2atom.find(v, a));
    SASSERT(a);
    SASSERT(ctx.get_assignment(v) != l_undef);
    SASSERT((ctx.get_assignment(v) == l_true) == is_true);
    a->assign_eh(is_true);

    // m_asserted_atoms.push_back(a);

    edge_id asserted_edge_id = a->get_asserted_edge();
    edge_id asserted_edge_pos_id = a->get_pos();
    edge_id asserted_edge_neg_id = a->get_neg();
    theory_var src = m_graph.get_source(asserted_edge_pos_id);
    theory_var tgt = m_graph.get_target(asserted_edge_pos_id);
    theory_var src_ = m_graph.get_source(asserted_edge_neg_id);
    theory_var tgt_ = m_graph.get_target(asserted_edge_neg_id);
    SASSERT ( src == tgt_ );
    SASSERT ( tgt == src_ );
    literal pos_exp = m_graph.get_explanation(asserted_edge_pos_id);
    literal neg_exp = m_graph.get_explanation(asserted_edge_neg_id);
    numeral pos_wgt = m_graph.get_weight(asserted_edge_pos_id);
    numeral neg_wgt = m_graph.get_weight(asserted_edge_neg_id);

    int src_idx = m_equation_elim.back_index(src);
    int tgt_idx = m_equation_elim.back_index(tgt);
    bool src_elim = m_equation_elim.contains(src);
    bool tgt_elim = m_equation_elim.contains(tgt);

    if (!m_asserted_atoms.empty()) {
        IF_VERBOSE(5, verbose_stream() << "W-DL: check EQUAL first\n";);
        atom * last_a = m_asserted_atoms.back();
        // both pos_edge should be selected
        if (last_a->is_true() && is_true) {
            IF_VERBOSE(5, verbose_stream() << "W-DL: previous and current are true\n";);
            bool_var prev_v = last_a->get_bool_var();
            atom * prev_a = nullptr;
            m_bool_var2atom.find(prev_v, prev_a);
            edge_id prev_edge_pos_id = prev_a->get_pos();
            // edge_id prev_edge_pos_id = last_a->get_pos();s
            theory_var prev_src = m_graph.get_source(prev_edge_pos_id);
            theory_var prev_tgt = m_graph.get_target(prev_edge_pos_id);
            numeral prev_wgt = m_graph.get_weight(prev_edge_pos_id);
            literal prev_exp = m_graph.get_explanation(prev_edge_pos_id);
            IF_VERBOSE(5, verbose_stream() << "W-DL: previous src and tgt: " << prev_src << " " << prev_tgt << "\n";);
            // check equal
            if ((prev_src == tgt) && (prev_tgt == src) && (prev_wgt + pos_wgt == numeral(0))) {
                IF_VERBOSE(5, verbose_stream() << "W-DL: EQUAL\n";);
                bool src_kept = m_equation_kept.contains(src);
                bool tgt_kept = m_equation_kept.contains(tgt);
                // // eliminate src
                // if (src_elim) {
                //     IF_VERBOSE(5, verbose_stream() << "W-DL: eliminable src, get src_old\n");
                //     theory_var src_old = m_equation_kept[src_idx];
                //     numeral src_wgt_old = m_equation_weight[src_idx];
                //     numeral new_wgt = src_wgt_old + pos_wgt;
                //     if (! tgt_kept) {
                //         IF_VERBOSE(5, verbose_stream() << "W-DL: elim tgt\n");
                //         m_equation_kept.push_back(src_old);
                //         m_equation_elim.push_back(tgt);
                //         m_equation_weight.push_back(new_wgt);
                //     }
                //     // else {
                //     //     IF_VERBOSE(5, verbose_stream() << "W-DL: keep both src_old and tgt\n");
                //     // }

                //     // // use new edges for this equation
                //     // edge_id prev_pos = m_graph.add_edge(tgt, src_old, numeral(0) - new_wgt, prev_exp);
                //     // edge_id new_pos = m_graph.add_edge(src_old, tgt, new_wgt, pos_exp);
                //     // atom * new_prev_a = alloc(atom, prev_v, prev_pos, null_edge_id);
                //     // atom * new_a = alloc(atom, v, new_pos, null_edge_id);

                //     // set the original edge
                //     m_asserted_atoms.push_back(a);
                //     // // set the updated edge
                //     // // m_asserted_atoms.pop_back();
                //     // m_atoms.push_back(new_prev_a);
                //     // m_atoms.push_back(new_a);
                //     // new_prev_a->assign_eh(is_true);
                //     // new_a->assign_eh(is_true);
                //     // m_asserted_atoms.push_back(new_prev_a);
                //     // m_asserted_atoms.push_back(new_a);
                // }
                // // eliminate tgt
                // else if (tgt_elim) {
                //     IF_VERBOSE(5, verbose_stream() << "W-DL: eliminable tgt, get tgt_old\n");
                //     theory_var tgt_old = m_equation_kept[tgt_idx];
                //     numeral tgt_wgt_old = m_equation_weight[tgt_idx];
                //     numeral new_wgt = tgt_wgt_old - pos_wgt;
                //     if (! src_kept) {
                //         IF_VERBOSE(5, verbose_stream() << "W-DL: elim src\n");
                //         m_equation_kept.push_back(tgt_old);
                //         m_equation_elim.push_back(src);
                //         m_equation_weight.push_back(new_wgt);
                //     }
                //     // else {
                //     //     IF_VERBOSE(5, verbose_stream() << "W-DL: keep both tgt_old and src\n");
                //     // }

                //     // // use new edges for this equation
                //     // edge_id prev_pos = m_graph.add_edge(src, tgt_old, numeral(0) - new_wgt, prev_exp);
                //     // edge_id new_pos = m_graph.add_edge(tgt_old, src, new_wgt, pos_exp);
                //     // atom * new_prev_a = alloc(atom, prev_v, prev_pos, null_edge_id);
                //     // atom * new_a = alloc(atom, v, new_pos, null_edge_id);

                //     // set the original edge
                //     m_asserted_atoms.push_back(a);
                //     // // set the updated edge
                //     // // m_asserted_atoms.pop_back();
                //     // m_atoms.push_back(new_prev_a);
                //     // m_atoms.push_back(new_a);
                //     // new_prev_a->assign_eh(is_true);
                //     // new_a->assign_eh(is_true);
                //     // m_asserted_atoms.push_back(new_prev_a);
                //     // m_asserted_atoms.push_back(new_a);
                // }
                // keep both
                // else
                if (!src_elim && !tgt_elim) {
                    IF_VERBOSE(5, verbose_stream() << "W-DL: not exist before\n");
                    if (pos_wgt == numeral(0)) {
                        if (src < tgt) {
                            if (!tgt_kept) {
                                IF_VERBOSE(5, verbose_stream() << "W-DL: 0 weight, elim tgt, keep src\n");
                                m_equation_kept.push_back(src);
                                m_equation_elim.push_back(tgt);
                                m_equation_weight.push_back(numeral(0));
                            }
                            else if (!src_kept) {
                                IF_VERBOSE(5, verbose_stream() << "W-DL: 0 weight, elim src, keep tgt\n");
                                m_equation_kept.push_back(tgt);
                                m_equation_elim.push_back(src);
                                m_equation_weight.push_back(numeral(0));
                            }
                        }
                        else if (tgt < src) {
                            if (!src_kept) {
                                IF_VERBOSE(5, verbose_stream() << "W-DL: 0 weight, elim src, keep tgt\n");
                                m_equation_kept.push_back(tgt);
                                m_equation_elim.push_back(src);
                                m_equation_weight.push_back(numeral(0));
                            }
                            else if (!tgt_kept) {
                                IF_VERBOSE(5, verbose_stream() << "W-DL: 0 weight, elim tgt, keep src\n");
                                m_equation_kept.push_back(src);
                                m_equation_elim.push_back(tgt);
                                m_equation_weight.push_back(numeral(0));
                            }
                        }
                    }
                    // target == source + (>0)
                    else if (pos_wgt > numeral(0)) {
                        if (!tgt_kept) {
                            IF_VERBOSE(5, verbose_stream() << "W-DL: positive weight, elim tgt, keep src\n");
                            m_equation_kept.push_back(src);
                            m_equation_elim.push_back(tgt);
                            m_equation_weight.push_back(pos_wgt);
                        }
                        else if (!src_kept) {
                            IF_VERBOSE(5, verbose_stream() << "W-DL: positive weight, elim src, keep tgt\n");
                            m_equation_kept.push_back(tgt);
                            m_equation_elim.push_back(src);
                            m_equation_weight.push_back(numeral(0) - pos_wgt);
                        }
                    }
                    // target = source + (<= 0); source == target + (>=0)
                    else {
                        if (!src_kept) {
                            IF_VERBOSE(5, verbose_stream() << "W-DL: negative weight, elim src, keep tgt\n");
                            m_equation_kept.push_back(tgt);
                            m_equation_elim.push_back(src);
                            m_equation_weight.push_back(numeral(0) - pos_wgt);
                        }
                        else if (!tgt_kept) {
                            IF_VERBOSE(5, verbose_stream() << "W-DL: negative weight, elim tgt, keep src\n");
                            m_equation_kept.push_back(src);
                            m_equation_elim.push_back(tgt);
                            m_equation_weight.push_back(pos_wgt);
                        }
                    }
                    // use original edges for this equation
                    m_asserted_atoms.push_back(a);
                }
                // eliminate both
                else {
                //     IF_VERBOSE(5, verbose_stream() << "W-DL: both eliminable\n");
                //     theory_var src_old = m_equation_kept[src_idx];
                //     theory_var tgt_old = m_equation_kept[tgt_idx];
                //     numeral src_equ_wgt = m_equation_weight[src_idx];
                //     numeral tgt_equ_wgt = m_equation_weight[tgt_idx];
                //     numeral new_wgt = pos_wgt + src_equ_wgt - tgt_equ_wgt;

                //     // use new edges for this equation
                //     // m_asserted_atoms.pop_back();
                //     edge_id prev_pos = m_graph.add_edge(tgt_old, src_old, numeral(0) - new_wgt, prev_exp);
                //     edge_id new_pos = m_graph.add_edge(src_old, tgt_old, new_wgt, pos_exp);
                //     atom * new_prev_a = alloc(atom, prev_v, prev_pos, null_edge_id);
                //     atom * new_a = alloc(atom, v, new_pos, null_edge_id);

                    // set the original edge
                    m_asserted_atoms.push_back(a);
                //     // set the updated edge
                //     m_atoms.push_back(new_prev_a);
                //     m_atoms.push_back(new_a);
                //     new_prev_a->assign_eh(is_true);
                //     new_a->assign_eh(is_true);
                //     m_asserted_atoms.push_back(new_prev_a);
                //     m_asserted_atoms.push_back(new_a);
                }
                IF_VERBOSE(1, verbose_stream() << "\nW-DL: added equation list display:\nkeep: " << m_equation_kept << "\nelim: " << m_equation_elim << "\nweig: "; display_equws(verbose_stream(), m_equation_weight););
                return;
            }
        }
        IF_VERBOSE(5, verbose_stream() << "W-DL: not EQUAL - previous is not a positive edge\n";);
    }

    // do `atom` replacement here
    // edge update
    // keep src and tgt 
    if (!src_elim && !tgt_elim) {
        IF_VERBOSE(5, verbose_stream() << "W-DL: keep src and tgt\n");
        
        // set the original edge
        m_asserted_atoms.push_back(a);

        // // set edge with -1 weight
        // // weight 0 or edge to 0 : use original atom
        // if (pos_wgt == numeral(0) || tgt == 0 || src == 0) {
        //     IF_VERBOSE(5, verbose_stream() << "W-DL: assign_eh - edge with weight 0 or edge to 0 - original atom\n";);
        //     m_asserted_atoms.push_back(a);
        // }
        // // pos_weg < 0, positive edge with -1
        // else if (pos_wgt < numeral(0)) {
        //     IF_VERBOSE(5, verbose_stream() << "W-DL: assign_eh - edge with weight [" << pos_wgt << "] < 0 - only pos_edge (weight " << pos_wgt << ") or neg_edge (weight 0)\n";);            
        //     edge_id pos = m_graph.add_edge(src, tgt, numeral(-1), pos_exp);
        //     edge_id neg;
        //     if (neg_wgt == numeral(0))
        //         neg = m_graph.add_edge(tgt, src, neg_wgt, neg_exp);
        //     else
        //         neg = null_edge_id;
        //     atom * new_a = alloc(atom, v, pos, neg);
        //     m_atoms.push_back(new_a);
        //     new_a->assign_eh(is_true);
        //     m_asserted_atoms.push_back(new_a);
        // }
        // // pos_wgt > 0; neg_wgt < 0
        // else {
        //     IF_VERBOSE(5, verbose_stream() << "W-DL: edge with weight [" << pos_wgt << "] > 0 - only neg_edge (weight " << neg_wgt << ")\n";);            
        //     edge_id pos = null_edge_id;
        //     edge_id neg = m_graph.add_edge(tgt, src, numeral(-1), neg_exp);
        //     atom * new_a = alloc(atom, v, pos, neg);
        //     m_atoms.push_back(new_a);
        //     new_a->assign_eh(is_true);
        //     m_asserted_atoms.push_back(new_a);
        // }
    }
    // keep src, eliminate tgt
    else if (!src_elim && tgt_elim) {
        IF_VERBOSE(5, verbose_stream() << "W-DL: elim tgt, keep src\n");
        theory_var tgt_old = m_equation_kept[tgt_idx];
        numeral equ_wgt = m_equation_weight[tgt_idx];
        numeral pos_wgt_new = pos_wgt - equ_wgt;
        numeral neg_wgt_new = neg_wgt + equ_wgt;
        edge_id pos; edge_id neg;

        // // original edges
        // pos = m_graph.add_edge(src, tgt, pos_wgt, pos_exp);
        // neg = m_graph.add_edge(tgt, src, neg_wgt, neg_exp);

        // set edge with correct weight 
        pos = m_graph.add_edge(src, tgt_old, pos_wgt_new, pos_exp);
        neg = m_graph.add_edge(tgt_old, src, neg_wgt_new, neg_exp);

        // // set edge with -1 weight
        // if (pos_wgt_new == numeral(0) || tgt_old == 0 || src == 0)
        //     pos = m_graph.add_edge(src, tgt_old, pos_wgt_new, pos_exp);
        // else if (pos_wgt_new < numeral(0))
        //     pos = m_graph.add_edge(src, tgt_old, numeral(-1), pos_exp);
        // else
        //     pos = null_edge_id;
        
        // if (neg_wgt_new == numeral(0) || tgt_old == 0 || src == 0)
        //     neg = m_graph.add_edge(tgt_old, src, neg_wgt_new, neg_exp);
        // else if (neg_wgt_new < numeral(0))
        //     neg = m_graph.add_edge(tgt_old, src, numeral(-1), neg_exp);
        // else
        //     neg = null_edge_id;
        
        // set the original edge
        m_asserted_atoms.push_back(a);
        // set the updated edge
        atom * new_a = alloc(atom, v, pos, neg);
        m_atoms.push_back(new_a);
        new_a->assign_eh(is_true);
        m_asserted_atoms.push_back(new_a);
    }

    // keep tgt, eliminate src
    else if (src_elim && !tgt_elim) {
        IF_VERBOSE(5, verbose_stream() << "W-DL: elim src, keep tgt\n");
        theory_var src_old = m_equation_kept[src_idx];
        numeral equ_wgt = m_equation_weight[src_idx];
        numeral pos_wgt_new = pos_wgt + equ_wgt;
        numeral neg_wgt_new = neg_wgt - equ_wgt;
        edge_id pos; edge_id neg;

        // // original edges
        // pos = m_graph.add_edge(src, tgt, pos_wgt, pos_exp);
        // neg = m_graph.add_edge(tgt, src, neg_wgt, neg_exp);

        // set edge with correct weight
        pos = m_graph.add_edge(src_old, tgt, pos_wgt_new, pos_exp);
        neg = m_graph.add_edge(tgt, src_old, neg_wgt_new, neg_exp);

        // // set edge with -1 weight
        // if (pos_wgt_new == numeral(0) || tgt == 0 || src_old == 0)
        //     pos = m_graph.add_edge(src_old, tgt, pos_wgt_new, pos_exp);
        // else if (pos_wgt_new < numeral(0))
        //     pos = m_graph.add_edge(src_old, tgt, numeral(-1), pos_exp);
        // else
        //     pos = null_edge_id;
        
        // if (neg_wgt_new == numeral(0) || tgt == 0 || src_old == 0)
        //     neg = m_graph.add_edge(tgt, src_old, neg_wgt_new, neg_exp);
        // else if (neg_wgt_new < numeral(0))
        //     neg = m_graph.add_edge(tgt, src_old, numeral(-1), neg_exp);
        // else
        //     neg = null_edge_id;
        
        // set the original edge
        m_asserted_atoms.push_back(a);
        // set the updated edge
        atom * new_a = alloc(atom, v, pos, neg);
        m_atoms.push_back(new_a);
        new_a->assign_eh(is_true);
        m_asserted_atoms.push_back(new_a);
    }

    // eliminate src and tgt
    // else {
    else if (src_elim && tgt_elim) {
        // SASSERT( src_idx != -1 );
        // SASSERT( tgt_idx != -1 );
        IF_VERBOSE(5, verbose_stream() << "W-DL: elim src and tgt\n");
        theory_var src_old = m_equation_kept[src_idx];
        theory_var tgt_old = m_equation_kept[tgt_idx];
        numeral src_equ_wgt = m_equation_weight[src_idx];
        numeral tgt_equ_wgt = m_equation_weight[tgt_idx];
        numeral pos_wgt_new = pos_wgt + src_equ_wgt - tgt_equ_wgt;
        numeral neg_wgt_new = neg_wgt - src_equ_wgt + tgt_equ_wgt;
        edge_id pos; edge_id neg;

        // // original edges
        // pos = m_graph.add_edge(src, tgt, pos_wgt, pos_exp);
        // neg = m_graph.add_edge(tgt, src, neg_wgt, neg_exp);

        // set edge with correct weight
        pos = m_graph.add_edge(src_old, tgt_old, pos_wgt_new, pos_exp);
        neg = m_graph.add_edge(tgt_old, src_old, neg_wgt_new, neg_exp);

        // // set edge with -1 weight
        // if (pos_wgt_new == numeral(0) || tgt_old == 0 || src_old == 0)
        //     pos = m_graph.add_edge(src_old, tgt_old, pos_wgt_new, pos_exp);
        // else if (pos_wgt_new < numeral(0))
        //     pos = m_graph.add_edge(src_old, tgt_old, numeral(-1), pos_exp);
        // else
        //     pos = null_edge_id;
        
        // if (neg_wgt_new == numeral(0) || tgt_old == 0 || src_old == 0)
        //     neg = m_graph.add_edge(tgt_old, src_old, neg_wgt_new, neg_exp);
        // else if (neg_wgt_new < numeral(0))
        //     neg = m_graph.add_edge(tgt_old, src_old, numeral(-1), neg_exp);
        // else
        //     neg = null_edge_id;
        
        // set the original edge
        m_asserted_atoms.push_back(a);
        // set the updated edge
        atom * new_a = alloc(atom, v, pos, neg);
        m_atoms.push_back(new_a);
        new_a->assign_eh(is_true);
        m_asserted_atoms.push_back(new_a);
    }
    else {
        IF_VERBOSE(5, verbose_stream() << "W-DL: old src and tgt, keep\n");
        // set the original edge
        m_asserted_atoms.push_back(a);
    }
    // IF_VERBOSE(15, verbose_stream() << "\nW-DL: dl-graph display:\n"; display(verbose_stream()); );
    IF_VERBOSE(5, verbose_stream() << "\nW-DL: equation list display:\nkeep: " << m_equation_kept << "\nelim: " << m_equation_elim << "\nweig: "; display_equws(verbose_stream(), m_equation_weight););
}
*/

/* WDL with weight relaxation */
template<typename Ext>
void theory_diff_logic_weak<Ext>::assign_eh(bool_var v, bool is_true) {
    IF_VERBOSE(5, verbose_stream() << "W-DL: assign_eh: " << v << ": " << is_true << "\n";);
    m_stats.m_num_assertions++;
    atom * a = nullptr;
    // if (m_bool_var2atom.find(v, a)) {
    VERIFY (m_bool_var2atom.find(v, a));
    SASSERT(a);
    SASSERT(ctx.get_assignment(v) != l_undef);
    SASSERT((ctx.get_assignment(v) == l_true) == is_true);
    a->assign_eh(is_true);

    // m_asserted_atoms.push_back(a);

    edge_id asserted_edge_id = a->get_asserted_edge();
    edge_id asserted_edge_pos_id = a->get_pos();
    edge_id asserted_edge_neg_id = a->get_neg();
    theory_var src = m_graph.get_source(asserted_edge_pos_id);
    theory_var tgt = m_graph.get_target(asserted_edge_pos_id);
    theory_var src_ = m_graph.get_source(asserted_edge_neg_id);
    theory_var tgt_ = m_graph.get_target(asserted_edge_neg_id);
    SASSERT ( src == tgt_ );
    SASSERT ( tgt == src_ );
    literal pos_exp = m_graph.get_explanation(asserted_edge_pos_id);
    literal neg_exp = m_graph.get_explanation(asserted_edge_neg_id);
    numeral pos_wgt = m_graph.get_weight(asserted_edge_pos_id);
    numeral neg_wgt = m_graph.get_weight(asserted_edge_neg_id);

    // // set the original edge
    // m_asserted_atoms.push_back(a);

    // always keep equations
    if (!m_asserted_atoms.empty()) {
        IF_VERBOSE(5, verbose_stream() << "W-DL: check EQUAL first\n";);
        atom * last_a = m_asserted_atoms.back();
        // both pos_edge should be selected
        if (last_a->is_true() && is_true) {
            IF_VERBOSE(5, verbose_stream() << "W-DL: previous and current are true\n";);
            bool_var prev_v = last_a->get_bool_var();
            atom * prev_a = nullptr;
            m_bool_var2atom.find(prev_v, prev_a);
            edge_id prev_edge_pos_id = prev_a->get_pos();
            // edge_id prev_edge_pos_id = last_a->get_pos();s
            theory_var prev_src = m_graph.get_source(prev_edge_pos_id);
            theory_var prev_tgt = m_graph.get_target(prev_edge_pos_id);
            numeral prev_wgt = m_graph.get_weight(prev_edge_pos_id);
            literal prev_exp = m_graph.get_explanation(prev_edge_pos_id);
            IF_VERBOSE(5, verbose_stream() << "W-DL: previous src and tgt: " << prev_src << " " << prev_tgt << "\n";);
            // check equal
            if ((prev_src == tgt) && (prev_tgt == src) && (prev_wgt + pos_wgt == numeral(0))) {
                IF_VERBOSE(5, verbose_stream() << "W-DL: EQUAL\n";);
                m_asserted_atoms.push_back(a);
                return;
            }
        }
    }
    // set edge with -1 weight
    // weight 0 or edge to 0 : use original atom
    if (pos_wgt == numeral(0) || tgt == 0 || src == 0) {
        IF_VERBOSE(5, verbose_stream() << "W-DL: assign_eh (weight relaxation) - edge with weight 0 or edge to 0 - original atom\n";);
        m_asserted_atoms.push_back(a);
    }
    // pos_weg < 0; keep positive edge -1; remove negative edge if weight != 0
    else if (pos_wgt < numeral(0)) {
        IF_VERBOSE(5, verbose_stream() << "W-DL: assign_eh (weight relaxation) - edge with weight [" << pos_wgt << "] < 0 - only pos_edge (weight " << pos_wgt << ") or neg_edge (weight 0)\n";);            
        edge_id pos = m_graph.add_edge(src, tgt, numeral(-1), pos_exp);
        edge_id neg;
        if (neg_wgt == numeral(0))
            neg = m_graph.add_edge(tgt, src, neg_wgt, neg_exp);
        else
            neg = null_edge_id;
        atom * new_a = alloc(atom, v, pos, neg);
        m_atoms.push_back(new_a);
        new_a->assign_eh(is_true);
        m_asserted_atoms.push_back(new_a);
    }
    // pos_wgt > 0, neg_wgt < 0; keep negative edge -1, remove positive edge
    else {
        IF_VERBOSE(5, verbose_stream() << "W-DL: assign_eh (weight relaxation) - edge with weight [" << pos_wgt << "] > 0 - only neg_edge (weight " << neg_wgt << ")\n";);            
        edge_id pos = null_edge_id;
        edge_id neg = m_graph.add_edge(tgt, src, numeral(-1), neg_exp);
        atom * new_a = alloc(atom, v, pos, neg);
        m_atoms.push_back(new_a);
        new_a->assign_eh(is_true);
        m_asserted_atoms.push_back(new_a);
    }
    IF_VERBOSE(15, verbose_stream() << "\nW-DL: dl-graph display:\n"; display(verbose_stream()); );
}


template<typename Ext>
void theory_diff_logic_weak<Ext>::collect_statistics(::statistics & st) const {
    st.update("w-dl conflicts", m_stats.m_num_conflicts);
    st.update("w-dl asserts", m_stats.m_num_assertions);
    st.update("core->w-dl eqs", m_stats.m_num_core2th_eqs);
    st.update("core->w-dl diseqs", m_stats.m_num_core2th_diseqs);
    m_arith_eq_adapter.collect_statistics(st);
    m_graph.collect_statistics(st);
}

template<typename Ext>
void theory_diff_logic_weak<Ext>::push_scope_eh() {
    IF_VERBOSE(5, verbose_stream() << "W-DL: push_scope_eh\n";);
    TRACE("arith", tout << "push\n";);
    theory::push_scope_eh();
    m_graph.push();
    SASSERT(m_equation_kept.size() == m_equation_elim.size());
    SASSERT(m_equation_kept.size() == m_equation_weight.size());
    m_scopes.push_back(scope());
    scope & s = m_scopes.back();
    s.m_atoms_lim = m_atoms.size();
    s.m_asserted_atoms_lim = m_asserted_atoms.size();
    s.m_equation_lim = m_equation_kept.size();
    s.m_asserted_qhead_old = m_asserted_qhead;
    s.m_equation_qhead_old = m_equation_qhead;
    IF_VERBOSE(5, verbose_stream() << "\nW-DL: m_asserted_qhead = " << m_asserted_qhead << ", total = " << m_asserted_atoms.size() << "\n";);
    IF_VERBOSE(5, verbose_stream() << "\nW-DL: equation list display:\nkeep: " << m_equation_kept << "\nelim: " << m_equation_elim << "\nweig: "; display_equws(verbose_stream(), m_equation_weight); verbose_stream() << "\nqhead: " << m_equation_qhead << "\n";);
}

template<typename Ext>
void theory_diff_logic_weak<Ext>::pop_scope_eh(unsigned num_scopes) {
    IF_VERBOSE(5, verbose_stream() << "W-DL: pop_scope_eh: " << num_scopes << "\n";);
    TRACE("arith", tout << "pop " << num_scopes << "\n";);
    unsigned lvl     = m_scopes.size();
    SASSERT(num_scopes <= lvl);
    unsigned new_lvl = lvl - num_scopes;
    scope & s        = m_scopes[new_lvl];
    del_atoms(s.m_atoms_lim);
    m_asserted_atoms.shrink(s.m_asserted_atoms_lim);
    m_asserted_qhead = s.m_asserted_qhead_old;
    m_equation_kept.shrink(s.m_equation_lim);
    m_equation_elim.shrink(s.m_equation_lim);
    m_equation_weight.shrink(s.m_equation_lim);
    m_equation_qhead = s.m_equation_qhead_old;
    m_scopes.shrink(new_lvl);
    unsigned num_edges = m_graph.get_num_edges();
    m_graph.pop(num_scopes);
    IF_VERBOSE(5, verbose_stream() << "\nW-DL: m_asserted_qhead = " << m_asserted_qhead << ", total = " << m_asserted_atoms.size() << "\n";);
    IF_VERBOSE(5, verbose_stream() << "\nW-DL: equation list display:\nkeep: " << m_equation_kept << "\nelim: " << m_equation_elim << "\nweig: "; display_equws(verbose_stream(), m_equation_weight); verbose_stream() << "\nqhead: " << m_equation_qhead << "\n";);
    IF_VERBOSE(15, verbose_stream() << "\nW-DL: dl-graph display:\n"; display(verbose_stream()); );
    CTRACE("arith", !m_graph.is_feasible_dbg(), m_graph.display(tout););
    if (num_edges != m_graph.get_num_edges() && m_num_simplex_edges > 0) {
        m_S.reset();
        m_num_simplex_edges = 0;
        m_objective_rows.reset();
    }
    theory::pop_scope_eh(num_scopes);
}

template<typename Ext>
final_check_status theory_diff_logic_weak<Ext>::final_check_eh() {
    IF_VERBOSE(5, verbose_stream() << "\nW-DL: final_check_eh\n";);
    if (can_propagate()) {
        propagate_core();
        IF_VERBOSE(5, verbose_stream() << "W-DL: final_check - can propagate, continue\n";);
        return FC_CONTINUE;
    }

    TRACE("arith_final", display(tout); );
    if (!is_consistent()) {
        IF_VERBOSE(5, verbose_stream() << "W-DL: final_check - not consistent, continue\n";);
        return FC_CONTINUE;
    }
    SASSERT(is_consistent());
    if (assume_eqs(m_var_value_table)) {
        IF_VERBOSE(5, verbose_stream() << "W-DL: final_check - assume_eqs, continue\n";);
        return FC_CONTINUE;
    }
    if (m_non_diff_logic_exprs) {
        IF_VERBOSE(5, verbose_stream() << "W-DL: final_check - non diff logic exprs, giveup\n";);
        return FC_GIVEUP; 
    }

    for (enode* n : ctx.enodes()) {
        family_id fid = n->get_owner()->get_family_id();
        if (fid != get_family_id() && 
            fid != m.get_basic_family_id() &&
            !is_uninterp_const(n->get_owner())) {
            TRACE("arith", tout << mk_pp(n->get_owner(), m) << "\n";);
            IF_VERBOSE(5, verbose_stream() << "W-DL: final_check - giveup on enode\n" << mk_pp(n->get_owner(), m) << "\n";);
            return FC_GIVEUP;
        }
    }
    
    // either will already be zero (as we don't do mixed constraints).
    m_graph.set_to_zero(get_zero(true), get_zero(false));

    IF_VERBOSE(5, verbose_stream() << "W-DL: final_check - done\n";);
    return FC_DONE;
}


template<typename Ext>
void theory_diff_logic_weak<Ext>::del_atoms(unsigned old_size) {
    typename atoms::iterator begin = m_atoms.begin() + old_size;
    typename atoms::iterator it    = m_atoms.end();
    while (it != begin) {
        --it;
        atom * a     = *it;
        bool_var bv  = a->get_bool_var();
        atom * orig_a = nullptr;
        SASSERT(m_bool_var2atom.find(bv, orig_a));
        if (orig_a == a) {
            m_bool_var2atom.erase(bv);
            dealloc(a);
        }
    }    
    m_atoms.shrink(old_size);
}


template<typename Ext>
bool theory_diff_logic_weak<Ext>::decompose_linear(app_ref_vector& terms, bool_vector& signs) {
    for (unsigned i = 0; i < terms.size(); ++i) {
        app* n = terms.get(i);
        bool sign;
        if (m_util.is_add(n)) {
            expr* arg = n->get_arg(0);
            if (!is_app(arg)) return false;
            expr_ref _n(n, m);
            terms[i] = to_app(arg);
            sign = signs[i];
            for (unsigned j = 1; j < n->get_num_args(); ++j) {
                arg = n->get_arg(j);
                if (!is_app(arg)) return false;
                terms.push_back(to_app(arg));
                signs.push_back(sign);
            }
            --i;
            continue;
        }
        expr* x, *y;
        if (m_util.is_mul(n, x, y)) {
            if (is_sign(x, sign) && is_app(y)) {
                terms[i] = to_app(y);
                signs[i] = (signs[i] == sign);
                --i;
            }
            else if (is_sign(y, sign) && is_app(x)) {
                terms[i] = to_app(x);
                signs[i] = (signs[i] == sign);
                --i;
            }
            continue;
        }
        if (m_util.is_uminus(n, x) && is_app(x)) {
            terms[i] = to_app(x);
            signs[i] = !signs[i];
            --i;
            continue;
        }
    }
    return true;
}

template<typename Ext>
bool theory_diff_logic_weak<Ext>::is_sign(expr* n, bool& sign) {
    IF_VERBOSE(15, verbose_stream() << "W-DL: is_sign: " << mk_pp(n, m) << "\n";);
    rational r;
    expr* x;
    if (m_util.is_numeral(n, r)) {
        if (r.is_one()) {
            sign = true;
            return true;
        }
        if (r.is_minus_one()) {
            sign = false;
            return true;
        }
    }
    else if (m_util.is_uminus(n, x)) {
        if (is_sign(x, sign)) {
            sign = !sign;
            return true;
        }
    }
    return false;
}

template<typename Ext>
bool theory_diff_logic_weak<Ext>::is_negative(app* n, app*& m) { 
    IF_VERBOSE(15, verbose_stream() << "W-DL: is_negative: " << mk_pp(n, get_manager()) << "\n";);
    expr* a0, *a1, *a2;
    rational r;
    if (!m_util.is_mul(n, a0, a1)) {
        return false;
    }
    if (m_util.is_numeral(a1)) {
        std::swap(a0, a1);
    }
    if (m_util.is_numeral(a0, r) && r.is_minus_one() && is_app(a1)) {
        m = to_app(a1);
        return true;
    }
    if (m_util.is_uminus(a1)) {
        std::swap(a0, a1);
    }
    if (m_util.is_uminus(a0, a2) && m_util.is_numeral(a2, r) && r.is_one() && is_app(a1)) {
        m = to_app(a1);
        return true;
    }
    return false;
}

template<typename Ext>
void theory_diff_logic_weak<Ext>::propagate() {
    if (m_params.m_arith_adaptive) {

        switch (m_params.m_arith_propagation_strategy) {

        case ARITH_PROP_PROPORTIONAL: {

            ++m_num_propagation_calls;
            if (m_num_propagation_calls * (m_stats.m_num_conflicts + 1) > 
                m_params.m_arith_adaptive_propagation_threshold * ctx.m_stats.m_num_conflicts) {
                m_num_propagation_calls = 1;
                TRACE("arith_prop", tout << "propagating: " << m_num_propagation_calls << "\n";);
                propagate_core();            
            }
            else {
                TRACE("arith_prop", tout << "skipping propagation " << m_num_propagation_calls << "\n";);
            }
            break;
        }
        case ARITH_PROP_AGILITY: {
            // update agility with factor generated by other conflicts.

            double g = m_params.m_arith_adaptive_propagation_threshold;
            while (m_num_core_conflicts < ctx.m_stats.m_num_conflicts) {
                m_agility = m_agility*g;
                ++m_num_core_conflicts;
            }        
            ++m_num_propagation_calls;
            bool do_propagate = (m_num_propagation_calls * m_agility > m_params.m_arith_adaptive_propagation_threshold);
            TRACE("arith_prop", tout << (do_propagate?"propagating: ":"skipping ") 
                  << " " << m_num_propagation_calls 
                  << " agility: " << m_agility << "\n";);
            if (do_propagate) {
                m_num_propagation_calls = 0;
                propagate_core();
            }
            break;
        }
        default:
            SASSERT(false);
            propagate_core();
        }
    }
    else {
        propagate_core();            
    }
}

template<typename Ext>
void theory_diff_logic_weak<Ext>::inc_conflicts() {
    ctx.push_trail(value_trail<context, bool>(m_consistent));
    m_consistent = false;
    m_stats.m_num_conflicts++;   
    if (m_params.m_arith_adaptive) {
        double g = m_params.m_arith_adaptive_propagation_threshold;
        m_agility = m_agility*g + 1 - g;
    }
}

template<typename Ext>
void theory_diff_logic_weak<Ext>::propagate_core() {
    IF_VERBOSE(5, verbose_stream() << "W-DL: propagate_core\n";);
    bool consistent = true;
    while (consistent && can_propagate()) {
        atom * a = m_asserted_atoms[m_asserted_qhead];
        m_asserted_qhead++;
        consistent = propagate_atom(a);
    }
}

template<typename Ext>
bool theory_diff_logic_weak<Ext>::propagate_atom(atom* a) {
    IF_VERBOSE(5, verbose_stream() << "W-DL: propagate_atom: atom = "; a->display(*this, verbose_stream()); verbose_stream() << "\n";);
    TRACE("arith", a->display(*this, tout); tout << "\n";);
    if (ctx.inconsistent()) {
        return false;
    }
    int edge_id = a->get_asserted_edge();
    if (!m_graph.enable_edge(edge_id)) {
        IF_VERBOSE(5, verbose_stream() << "W-DL: propagate_atom: before set_neg_cycle_conflict\n"; display(verbose_stream()); verbose_stream() << "\n";);
        TRACE("arith", display(tout););
        set_neg_cycle_conflict();
        
        return false;
    }
    return true;
}

template<typename Ext>
void theory_diff_logic_weak<Ext>::new_edge(dl_var src, dl_var dst, unsigned num_edges, edge_id const* edges) {
    if (!theory_resolve()) {
        return;
    }

    TRACE("dl_activity", tout << "\n";);

    numeral w(0);
    for (unsigned i = 0; i < num_edges; ++i) {
        w += m_graph.get_weight(edges[i]);
    }
    enode* e1 = get_enode(src);
    enode* e2 = get_enode(dst);
    expr*  n1 = e1->get_owner();
    expr*  n2 = e2->get_owner();
    bool is_int = m_util.is_int(n1);
    rational num = w.get_rational().to_rational();

    expr_ref le(m);
    if (w.is_rational()) {
        // x - y <= w
        expr*  n3 = m_util.mk_numeral(num, is_int);
        n2 = m_util.mk_mul(m_util.mk_numeral(rational(-1), is_int), n2);
        le = m_util.mk_le(m_util.mk_add(n1,n2), n3);
    }
    else {
        //     x - y < w 
        // <=> 
        //     not (x - y >= w)
        // <=>
        //     not (y - x <= -w)
        //
        SASSERT(w.get_infinitesimal().is_neg());
        expr*  n3 = m_util.mk_numeral(-num, is_int);
        n1 = m_util.mk_mul(m_util.mk_numeral(rational(-1), is_int), n1);
        le = m_util.mk_le(m_util.mk_add(n2,n1), n3);
        le = m.mk_not(le);
    }
    if (m.has_trace_stream())log_axiom_instantiation(le);
    ctx.internalize(le, false);
    if (m.has_trace_stream()) m.trace_stream() << "[end-of-instance]\n";
    ctx.mark_as_relevant(le.get());
    literal lit(ctx.get_literal(le));
    bool_var bv = lit.var();
    atom* a = nullptr;
    m_bool_var2atom.find(bv, a);
    SASSERT(a);

    literal_vector lits;
    for (unsigned i = 0; i < num_edges; ++i) {
        lits.push_back(~m_graph.get_explanation(edges[i]));        
    }
    lits.push_back(lit);

    IF_VERBOSE(5, verbose_stream() << "W-DL: new_edge:\n" << mk_pp(le, m) << "\n" << "edge: " << a->get_pos() << "\n"; ctx.display_literals_verbose(verbose_stream(), lits.size(), lits.c_ptr()); verbose_stream() << "\n"; );
    TRACE("dl_activity", 
          tout << mk_pp(le, m) << "\n";
          tout << "edge: " << a->get_pos() << "\n";
          ctx.display_literals_verbose(tout, lits.size(), lits.c_ptr());
          tout << "\n";
          );

    justification * js = nullptr;
    if (m.proofs_enabled()) {
        vector<parameter> params;
        params.push_back(parameter(symbol("farkas")));
        params.resize(lits.size()+1, parameter(rational(1)));
        js = new (ctx.get_region()) theory_lemma_justification(get_id(), ctx, 
                   lits.size(), lits.c_ptr(), 
                   params.size(), params.c_ptr());
    }
    ctx.mk_clause(lits.size(), lits.c_ptr(), js, CLS_TH_LEMMA, nullptr);
    if (dump_lemmas()) {
        symbol logic(m_lia_or_lra == is_lia ? "QF_LIA" : "QF_LRA");
        ctx.display_lemma_as_smt_problem(lits.size(), lits.c_ptr(), false_literal, logic);
    }

#if 0
    TRACE("arith",
          tout << "shortcut:\n";
          for (unsigned i = 0; i < num_edges; ++i) {
              edge_id e = edges[i];
              // tgt <= src + w
              numeral w = m_graph.get_weight(e);
              dl_var tgt = m_graph.get_target(e);
              dl_var src = m_graph.get_source(e);
              if (i + 1 < num_edges) {
                  dl_var tgt2 = m_graph.get_target(edges[i+1]);
                  SASSERT(src == tgt2);
              }        
              tout << "$" << tgt << " <= $" << src << " + " << w << "\n";
          }
          {
              numeral w = m_graph.get_weight(e_id);
              dl_var tgt = m_graph.get_target(e_id);
              dl_var src = m_graph.get_source(e_id);
              tout << "$" << tgt << " <= $" << src << " + " << w << "\n";
          }
          );
#endif

}

template<typename Ext>
void theory_diff_logic_weak<Ext>::set_neg_cycle_conflict() {
    m_nc_functor.reset();
    m_graph.traverse_neg_cycle2(m_params.m_arith_stronger_lemmas, m_nc_functor);
    inc_conflicts();
    literal_vector const& lits = m_nc_functor.get_lits();
    IF_VERBOSE(1, verbose_stream() << "W-DL: neg_cycle_conflict:\n"; for (literal lit : lits) ctx.display_literal_info(verbose_stream(), lit); verbose_stream() << "\n";);
    TRACE("arith_conflict", 
          tout << "conflict: ";
          for (literal lit : lits) ctx.display_literal_info(tout, lit);          
          tout << "\n";);

    if (dump_lemmas()) {
        symbol logic(m_lia_or_lra == is_lia ? "QF_LIA" : "QF_LRA");
        ctx.display_lemma_as_smt_problem(lits.size(), lits.c_ptr(), false_literal, logic);
    }

    vector<parameter> params;
    if (m.proofs_enabled()) {
        params.push_back(parameter(symbol("farkas")));
        for (unsigned i = 0; i <= lits.size(); ++i) {
            params.push_back(parameter(rational(1)));
        }
    } 
   
    ctx.set_conflict(
        ctx.mk_justification(
            ext_theory_conflict_justification(
                get_id(), ctx.get_region(), 
                lits.size(), lits.c_ptr(), 0, nullptr, params.size(), params.c_ptr())));

}

template<typename Ext>
bool theory_diff_logic_weak<Ext>::is_offset(app* n, app*& v, app*& offset, rational& r) {
    IF_VERBOSE(15, verbose_stream() << "W-DL: is_offset: " << mk_pp(n, m) << "\n"; );
    if (!m_util.is_add(n)) {
        return false;
    }

    if (n->get_num_args() == 2 && m_util.is_numeral(n->get_arg(0), r)) {
        v = to_app(n->get_arg(1));
        offset = to_app(n->get_arg(0));
        return true;
    }
    if (n->get_num_args() == 2 && m_util.is_numeral(n->get_arg(1), r)) {
        v = to_app(n->get_arg(0));
        offset = to_app(n->get_arg(1));
        return true;
    }
    return false;
}

template<typename Ext>
theory_var theory_diff_logic_weak<Ext>::mk_term(app* n) {
    IF_VERBOSE(5, verbose_stream() << "W-DL: mk_term: " << mk_pp(n, m) << "\n"; );
    SASSERT(!m_util.is_sub(n));
    SASSERT(!m_util.is_uminus(n));
    app* a, *offset;
    theory_var source, target;
    enode* e;

    TRACE("arith", tout << mk_pp(n, m) << "\n";);

    rational r;
    if (m_util.is_numeral(n, r)) {
        return mk_num(n, r);
    }
    else if (is_offset(n, a, offset, r)) {
        // n = a + k
        source = mk_var(a);
        for (unsigned i = 0; i < n->get_num_args(); ++i) {
            expr* arg = n->get_arg(i);
            if (!ctx.e_internalized(arg)) {
                ctx.internalize(arg, false);
            }
        }
        e = ctx.mk_enode(n, false, false, true);
        target = mk_var(e);
        numeral k(r);
        IF_VERBOSE(5, verbose_stream() << "W-DL: enabled_edge with weight: " << k << "\n";);
        m_graph.enable_edge(m_graph.add_edge(source, target, k, null_literal));
        m_graph.enable_edge(m_graph.add_edge(target, source, -k, null_literal));
        IF_VERBOSE(5, verbose_stream() << "\nW-DL: graph display:\n"; display(verbose_stream()); );
        return target;
    }
    else if (m_util.is_arith_expr(n)) {
        return null_theory_var;
    }
    else {
        return mk_var(n);
    }
}


template<typename Ext>
theory_var theory_diff_logic_weak<Ext>::mk_num(app* n, rational const& r) {
    IF_VERBOSE(15, verbose_stream() << "W-DL: mk_num: " << mk_pp(n, m) << "\n"; );
    theory_var v = null_theory_var;
    enode* e = nullptr;
    if (r.is_zero()) {
        v = get_zero(m_util.is_int(n));
    }
    else if (ctx.e_internalized(n)) {
        e = ctx.get_enode(n);
        v = e->get_th_var(get_id());
        SASSERT(v != null_theory_var);
    }
    else {
        theory_var zero = get_zero(m_util.is_int(n));
        SASSERT(n->get_num_args() == 0);
        e = ctx.mk_enode(n, false, false, true);
        v = mk_var(e);
        // internalizer is marking enodes as interpreted whenever the associated ast is a value and a constant.
        // e->mark_as_interpreted();
        numeral k(r);
        IF_VERBOSE(5, verbose_stream() << "W-DL: enabled_edge with weight: " << k << "\n";);
        m_graph.enable_edge(m_graph.add_edge(zero, v, k, null_literal));
        m_graph.enable_edge(m_graph.add_edge(v, zero, -k, null_literal));
        IF_VERBOSE(5, verbose_stream() << "\nW-DL: graph display:\n"; display(verbose_stream()); );
    }
    return v;
}


template<typename Ext>
theory_var theory_diff_logic_weak<Ext>::mk_var(enode* n) {
    theory_var v = theory::mk_var(n);
    IF_VERBOSE(5, verbose_stream() << "W-DL: enode mk_var: " << v << "\n";);
    TRACE("diff_logic_vars", tout << "mk_var: " << v << "\n";);
    m_graph.init_var(v);
    ctx.attach_th_var(n, this, v);
    set_sort(n->get_owner());
    return v;
}

template<typename Ext>
void theory_diff_logic_weak<Ext>::set_sort(expr* n) {
    IF_VERBOSE(15, verbose_stream() << "W-DL: set_sort: " << mk_pp(n, m) << "\n"; );
    if (m_util.is_numeral(n))
        return;
    if (m_util.is_int(n)) {
        if (m_lia_or_lra == is_lra) {
            throw default_exception("difference logic does not work with mixed sorts");
        }
        m_lia_or_lra = is_lia;
    }
    else {
        if (m_lia_or_lra == is_lia) {
            throw default_exception("difference logic does not work with mixed sorts");
        }
        m_lia_or_lra = is_lra;
    }
}


template<typename Ext>
theory_var theory_diff_logic_weak<Ext>::mk_var(app* n) {
    IF_VERBOSE(15, verbose_stream() << "W-DL: mk_var starts: " << mk_pp(n, m) << "\n";);
    enode* e = nullptr;
    theory_var v = null_theory_var;
    if (!ctx.e_internalized(n)) {
        ctx.internalize(n, false);
    }
    e = ctx.get_enode(n);
    v = e->get_th_var(get_id());

    if (v == null_theory_var) {
        v = mk_var(e);
    }      
    if (is_interpreted(n)) {
        TRACE("non_diff_logic", tout << "Variable should not be interpreted\n";);
        found_non_diff_logic_expr(n);
    }
    IF_VERBOSE(5, verbose_stream() << "W-DL: mk_var returns: " << mk_pp(n, m) << " |-> " << v << "\n";);
    TRACE("arith", tout << mk_pp(n, m) << " |-> " << v << "\n";);
    return v;
}




template<typename Ext>
void theory_diff_logic_weak<Ext>::reset_eh() {
    for (unsigned i = 0; i < m_atoms.size(); ++i) {
        dealloc(m_atoms[i]);
    }
    m_graph            .reset();
    m_izero            = null_theory_var;
    m_rzero            = null_theory_var;
    m_atoms            .reset();
    m_asserted_atoms   .reset();
    m_equation_kept    .reset();
    m_equation_elim    .reset();
    m_equation_weight  .reset();
    m_stats            .reset();
    m_scopes           .reset();
    m_equation_qhead        = 0;
    m_asserted_qhead        = 0;
    m_num_core_conflicts    = 0;
    m_num_propagation_calls = 0;
    m_agility               = 0.5;
    m_lia_or_lra            = not_set;
    m_non_diff_logic_exprs  = false;
    m_objectives      .reset();
    m_objective_consts.reset();
    m_objective_assignments.reset();
    theory::reset_eh();
}


template<typename Ext>
void theory_diff_logic_weak<Ext>::compute_delta() {
    IF_VERBOSE(5, verbose_stream() << "W-DL: compute_delta\n"; );
    m_delta = rational(1);
    m_graph.set_to_zero(get_zero(true), get_zero(false));
    unsigned num_edges = m_graph.get_num_edges();
    for (unsigned i = 0; i < num_edges; ++i) {
        if (!m_graph.is_enabled(i)) {
            continue;
        }
        numeral w = m_graph.get_weight(i);
        dl_var tgt = m_graph.get_target(i);
        dl_var src = m_graph.get_source(i);
        rational n_x = m_graph.get_assignment(tgt).get_rational().to_rational();
        rational k_x = m_graph.get_assignment(tgt).get_infinitesimal().to_rational();
        rational n_y = m_graph.get_assignment(src).get_rational().to_rational();
        rational k_y = m_graph.get_assignment(src).get_infinitesimal().to_rational();
        rational n_c = w.get_rational().to_rational();
        rational k_c = w.get_infinitesimal().to_rational();
        IF_VERBOSE(15, verbose_stream() << "(n_x,k_x): " << n_x << ", " << k_x << ", (n_y,k_y): " << n_y << ", " << k_y << ", (n_c,k_c): " << n_c << ", " << k_c << "\n";);
        TRACE("arith", tout << "(n_x,k_x): " << n_x << ", " << k_x << ", (n_y,k_y): " 
              << n_y << ", " << k_y << ", (n_c,k_c): " << n_c << ", " << k_c << "\n";);
        if (n_x < n_y + n_c && k_x > k_y + k_c) {
            rational new_delta = (n_y + n_c - n_x) / (2*(k_x - k_y - k_c));
            if (new_delta < m_delta) {
                IF_VERBOSE(15, verbose_stream() << "new delta: " << new_delta << "\n";);
                TRACE("arith", tout << "new delta: " << new_delta << "\n";);
                m_delta = new_delta;
            }
        }
    }
}



template<typename Ext>
void theory_diff_logic_weak<Ext>::init_model(smt::model_generator & m) {
    IF_VERBOSE(5, verbose_stream() << "W-DL: init_model\n";);
    m_factory = alloc(arith_factory, get_manager());
    m.register_factory(m_factory);
    compute_delta();
}
        

template<typename Ext>
model_value_proc * theory_diff_logic_weak<Ext>::mk_value(enode * n, model_generator & mg) {
    theory_var v = n->get_th_var(get_id());
    IF_VERBOSE(5, verbose_stream() << "W-DL: enode mk_value: " << v << "\n";);
    SASSERT(v != null_theory_var);
    rational num;
    if (!m_util.is_numeral(n->get_owner(), num)) {
        numeral val = m_graph.get_assignment(v);
        num = val.get_rational().to_rational() + m_delta * val.get_infinitesimal().to_rational();
    }
    TRACE("arith", tout << mk_pp(n->get_owner(), m) << " |-> " << num << "\n";);
    bool is_int = m_util.is_int(n->get_owner());
    if (is_int && !num.is_int())
        throw default_exception("difference logic solver was used on mixed int/real problem");
    return alloc(expr_wrapper_proc, m_factory->mk_num_value(num, is_int));
}


template<typename Ext>
void theory_diff_logic_weak<Ext>::display(std::ostream & out) const {
    out << "atoms\n";
    for (atom* a : m_atoms) {
        a->display(*this, out) << "\n";
    }
    out << "graph\n";
    m_graph.display(out);
}

template<typename Ext>
void theory_diff_logic_weak<Ext>::display_equws(std::ostream& out, svector<numeral> ws) const {
    for (numeral w: ws) out << w << " ";
    out << "\n";
}

template<typename Ext>
bool theory_diff_logic_weak<Ext>::is_consistent() const {    
    DEBUG_CODE(
        for (unsigned i = 0; m_graph.is_feasible_dbg() && i < m_atoms.size(); ++i) {
            atom* a = m_atoms[i];
            bool_var bv = a->get_bool_var();
            lbool asgn = ctx.get_assignment(bv);        
            if (ctx.is_relevant(ctx.bool_var2expr(bv)) && asgn != l_undef) {
                SASSERT((asgn == l_true) == a->is_true());
                int edge_id = a->get_asserted_edge();
                SASSERT(m_graph.is_enabled(edge_id));
                SASSERT(m_graph.is_feasible(edge_id));
            }
        });
    // IF_VERBOSE(5, verbose_stream() << "W-DL: is_consistent check done\n";);
    return m_consistent;
}


template<class Ext>
theory_var theory_diff_logic_weak<Ext>::expand(bool pos, theory_var v, rational & k) {
    IF_VERBOSE(5, verbose_stream() << "W-DL: enode expand: " << v << "\n";);
    enode* e = get_enode(v);
    rational r;
    for (;;) {
        app* n = e->get_owner();
        if (m_util.is_add(n) && n->get_num_args() == 2) {
            app* x = to_app(n->get_arg(0));
            app* y = to_app(n->get_arg(1));
            if (m_util.is_numeral(x, r)) {
                e = ctx.get_enode(y);                
            }
            else if (m_util.is_numeral(y, r)) {
                e = ctx.get_enode(x);
            }
            v = e->get_th_var(get_id());
            SASSERT(v != null_theory_var);
            if (v == null_theory_var) {
                break;
            }
            if (pos) {
                k += r;
            }
            else {
                k -= r;
            }
        }
        else {
            break;
        }
    }
    return v;
}

template<typename Ext>
void theory_diff_logic_weak<Ext>::new_eq_or_diseq(bool is_eq, theory_var v1, theory_var v2, justification& eq_just) {
    IF_VERBOSE(5, verbose_stream() << "W-DL: new_eq_or_diseq - " << is_eq << " : " << v1 << ", " << v2 << "\n";);
    rational k;
    theory_var s = expand(true,  v1, k);
    theory_var t = expand(false, v2, k);

    if (s == t) {
        if (is_eq != k.is_zero()) {
            // conflict 0 /= k;
            inc_conflicts();
            ctx.set_conflict(&eq_just);            
        }
    }
    else {
        //
        // Create equality ast, internalize_atom
        // assign the corresponding equality literal.
        //
        app_ref eq(m), s2(m), t2(m);
        app* s1 = get_enode(s)->get_owner();
        app* t1 = get_enode(t)->get_owner();
        s2 = m_util.mk_sub(t1, s1);
        t2 = m_util.mk_numeral(k, m.get_sort(s2.get()));
        // t1 - s1 = k
        eq = m.mk_eq(s2.get(), t2.get());
        if (m.has_trace_stream()) {
            app_ref body(m);
            body = m.mk_eq(m.mk_eq(m_util.mk_add(s1, t2), t1), eq);
            log_axiom_instantiation(body);
        }
        
        IF_VERBOSE(15, verbose_stream() << v1 << " .. " << v2 << "\n" << mk_pp(eq.get(), m) << "\n";);
        TRACE("diff_logic", 
              tout << v1 << " .. " << v2 << "\n";
              tout << mk_pp(eq.get(), m) <<"\n";);

        if (!internalize_atom(eq.get(), false)) {
            UNREACHABLE();
        }

        if (m.has_trace_stream()) m.trace_stream() << "[end-of-instance]\n";
                
        literal l(ctx.get_literal(eq.get()));
        if (!is_eq) {
            l = ~l;
        }

        ctx.assign(l, b_justification(&eq_just), false);
    }
}

template<typename Ext>
void theory_diff_logic_weak<Ext>::new_eq_eh(
    theory_var v1, theory_var v2, justification& j) {
    m_stats.m_num_core2th_eqs++;
    new_eq_or_diseq(true, v1, v2, j);
}


template<typename Ext>
void theory_diff_logic_weak<Ext>::new_diseq_eh(
    theory_var v1, theory_var v2, justification& j) {
    m_stats.m_num_core2th_diseqs++;
    new_eq_or_diseq(false, v1, v2, j);    
}


template<typename Ext>
void theory_diff_logic_weak<Ext>::new_eq_eh(theory_var v1, theory_var v2) {
    m_arith_eq_adapter.new_eq_eh(v1, v2);
}


template<typename Ext>
void theory_diff_logic_weak<Ext>::new_diseq_eh(theory_var v1, theory_var v2) {
    m_arith_eq_adapter.new_diseq_eh(v1, v2);
}



struct imp_functor {
    conflict_resolution & m_cr;
    imp_functor(conflict_resolution& cr) : m_cr(cr) {}
    void operator()(literal l) {
        m_cr.mark_literal(l);
    }
};

template<typename Ext>
void theory_diff_logic_weak<Ext>::get_eq_antecedents(
    theory_var v1, theory_var v2, unsigned timestamp, conflict_resolution & cr) {
    imp_functor functor(cr);
    VERIFY(m_graph.find_shortest_zero_edge_path(v1, v2, timestamp, functor));
    VERIFY(m_graph.find_shortest_zero_edge_path(v2, v1, timestamp, functor));
}

template<typename Ext>
void theory_diff_logic_weak<Ext>::get_implied_bound_antecedents(edge_id bridge_edge, edge_id subsumed_edge, conflict_resolution & cr) {
    imp_functor f(cr);
    m_graph.explain_subsumed_lazy(bridge_edge, subsumed_edge, f);
}

template<typename Ext>
unsigned theory_diff_logic_weak<Ext>::node2simplex(unsigned v) {
    return m_objectives.size() + 2*v + 1;
}
template<typename Ext>
unsigned theory_diff_logic_weak<Ext>::edge2simplex(unsigned e) {
    return m_objectives.size() + 2*e;
}
template<typename Ext>
unsigned theory_diff_logic_weak<Ext>::obj2simplex(unsigned e) {
    return e;
}

template<typename Ext>
unsigned theory_diff_logic_weak<Ext>::num_simplex_vars() {
    return m_objectives.size() + std::max(2*m_graph.get_num_edges(),2*m_graph.get_num_nodes()+1);
}

template<typename Ext>
bool theory_diff_logic_weak<Ext>::is_simplex_edge(unsigned e) {
    if (e < m_objectives.size()) return false;
    e -= m_objectives.size();
    return (0 == (e & 0x1));
}

template<typename Ext> 
unsigned theory_diff_logic_weak<Ext>::simplex2edge(unsigned e) {
    SASSERT(is_simplex_edge(e));
    return (e - m_objectives.size())/2;
}

template<typename Ext> 
void theory_diff_logic_weak<Ext>::update_simplex(Simplex& S) {
    IF_VERBOSE(5, verbose_stream() << "W-DL: update_simplex\n";);
    m_graph.set_to_zero(get_zero(true), get_zero(false));
    unsynch_mpq_inf_manager inf_mgr;
    unsynch_mpq_manager& mgr = inf_mgr.get_mpq_manager();
    unsigned num_nodes = m_graph.get_num_nodes();
    vector<dl_edge<GExt> > const& es = m_graph.get_all_edges();
    S.ensure_var(num_simplex_vars());
    for (unsigned i = 0; i < num_nodes; ++i) {
        numeral const& a = m_graph.get_assignment(i);
        rational fin = a.get_rational().to_rational();
        rational inf = a.get_infinitesimal().to_rational();
        mpq_inf q;
        inf_mgr.set(q, fin.to_mpq(), inf.to_mpq());
        S.set_value(node2simplex(i), q);
        inf_mgr.del(q);
    }
    S.set_lower(node2simplex(get_zero(true)), mpq_inf(mpq(0), mpq(0)));
    S.set_upper(node2simplex(get_zero(true)), mpq_inf(mpq(0), mpq(0)));
    S.set_lower(node2simplex(get_zero(false)), mpq_inf(mpq(0), mpq(0)));
    S.set_upper(node2simplex(get_zero(false)), mpq_inf(mpq(0), mpq(0)));
    svector<unsigned> vars;
    scoped_mpq_vector coeffs(mgr);
    coeffs.push_back(mpq(1));
    coeffs.push_back(mpq(-1));
    coeffs.push_back(mpq(-1));
    vars.resize(3);
    for (unsigned i = m_num_simplex_edges; i < es.size(); ++i) {
        //    t - s <= w 
        // =>
        //    t - s - b = 0, b >= w
        dl_edge<GExt> const& e = es[i];
        unsigned base_var = edge2simplex(i);
        vars[0] = node2simplex(e.get_target());
        vars[1] = node2simplex(e.get_source());
        vars[2] = base_var;
        S.add_row(base_var, 3, vars.c_ptr(), coeffs.c_ptr());        
    }
    m_num_simplex_edges = es.size();
    for (unsigned i = 0; i < es.size(); ++i) {
        dl_edge<GExt> const& e = es[i];
        unsigned base_var = edge2simplex(i);
        if (e.is_enabled()) {
            numeral const& w = e.get_weight();
            rational fin = w.get_rational().to_rational();
            rational inf = w.get_infinitesimal().to_rational();
            mpq_inf q;
            inf_mgr.set(q, fin.to_mpq(), inf.to_mpq());
            S.set_upper(base_var, q);
            inf_mgr.del(q);
        }
        else {
            S.unset_upper(base_var);
        }
    }
    for (unsigned v = m_objective_rows.size(); v < m_objectives.size(); ++v) {
        unsigned w = obj2simplex(v);
        objective_term const& objective = m_objectives[v];

        // add objective function as row.
        coeffs.reset();
        vars.reset();
        for (auto const& o : objective) {
            coeffs.push_back(o.second.to_mpq());
            vars.push_back(node2simplex(o.first));
        }
        coeffs.push_back(mpq(1));
        vars.push_back(w);
        Simplex::row row = S.add_row(w, vars.size(), vars.c_ptr(), coeffs.c_ptr());
        m_objective_rows.push_back(row);
    }
}

template<typename Ext>
typename theory_diff_logic_weak<Ext>::inf_eps theory_diff_logic_weak<Ext>::value(theory_var v) {
    IF_VERBOSE(5, verbose_stream() << "W-DL: value: " << v << "\n";);
     objective_term const& objective = m_objectives[v];   
     inf_eps r = inf_eps(m_objective_consts[v]);
     for (auto const& o : objective) {
         numeral n = m_graph.get_assignment(o.first);
         rational r1 = n.get_rational().to_rational();
         rational r2 = n.get_infinitesimal().to_rational();
         r += o.second * inf_eps(rational(0), inf_rational(r1, r2));
     }
     return r;
}



template<typename Ext>
typename theory_diff_logic_weak<Ext>::inf_eps 
theory_diff_logic_weak<Ext>::maximize(theory_var v, expr_ref& blocker, bool& has_shared) {
    IF_VERBOSE(5, verbose_stream() << "W-DL: maximize: " << v << "\n";);
    SASSERT(is_consistent());

    has_shared = false;
    Simplex& S = m_S;

    CTRACE("arith",!m_graph.is_feasible_dbg(), m_graph.display(tout););
    SASSERT(m_graph.is_feasible_dbg());

    update_simplex(S);

    TRACE("arith",
          objective_term const& objective = m_objectives[v];
          for (auto const& o : objective) {
              tout << "Coefficient " << o.second 
                   << " of theory_var " << o.first << "\n";
          }
          tout << "Free coefficient " << m_objective_consts[v] << "\n";
          );

    IF_VERBOSE(5, S.display(verbose_stream()); for (unsigned i = 0; i < m_graph.get_num_nodes(); ++i) verbose_stream() << "$" << i << ": " << node2simplex(i) << "\n"; display(verbose_stream()); );
    TRACE("opt", 
          S.display(tout); 
          for (unsigned i = 0; i < m_graph.get_num_nodes(); ++i)
              tout << "$" << i << ": " << node2simplex(i) << "\n";
          display(tout);
          );
    
    // optimize    
    lbool is_sat = S.make_feasible();
    if (is_sat == l_undef) {
        blocker = m.mk_false();
        return inf_eps::infinity();        
    }
    // IF_VERBOSE(5, S.display(verbose_stream()); );
    TRACE("opt", S.display(tout); );    
    SASSERT(is_sat != l_false);
    unsigned w = obj2simplex(v);
    lbool is_fin = S.minimize(w);
    switch (is_fin) {
    case l_true: {
        simplex::mpq_ext::eps_numeral const& val = S.get_value(w);
        inf_rational r(-rational(val.first), -rational(val.second));
        Simplex::row row = m_objective_rows[v];
        Simplex::row_iterator it = S.row_begin(row), end = S.row_end(row);
        expr_ref_vector& core = m_objective_assignments[v];
        expr_ref tmp(m);
        core.reset();
        for (; it != end; ++it) {
            unsigned v = it->m_var;
            if (is_simplex_edge(v)) {
                unsigned edge_id = simplex2edge(v);
                literal lit = m_graph.get_explanation(edge_id);
                if (lit != null_literal) {
                    ctx.literal2expr(lit, tmp);
                    core.push_back(tmp);
                }
            }
        }
        ensure_rational_solution(S);
        TRACE("opt", tout << r << " " << "\n"; 
              S.display_row(tout, row, true);
              S.display(tout);
              );

        for (unsigned i = 0; i < m_graph.get_num_nodes(); ++i) {
            unsigned w = node2simplex(i);
            auto const& val = S.get_value(w);
            SASSERT(rational(val.second).is_zero());
            rational r = rational(val.first);
            m_graph.set_assignment(i, numeral(r));
        }
        CTRACE("arith",!m_graph.is_feasible_dbg(), m_graph.display(tout););
        SASSERT(m_graph.is_feasible_dbg());
        inf_eps r1(rational(0), r);
        blocker = mk_gt(v, r1);
        return inf_eps(rational(0), r + m_objective_consts[v]);
    }
    default:
        TRACE("opt", tout << "unbounded\n"; );        
        blocker = m.mk_false();
        return inf_eps::infinity();        
    }
}

template<typename Ext>
theory_var theory_diff_logic_weak<Ext>::add_objective(app* term) {
    IF_VERBOSE(15, verbose_stream() << "W-DL: add_objective: " << mk_pp(term, m) << "\n";);
    objective_term objective;
    theory_var result = m_objectives.size();
    rational q(1), r(0);
    expr_ref_vector vr(m);
    if (!is_linear(m, term)) {
        result = null_theory_var;
    }
    else if (internalize_objective(term, q, r, objective)) {
        m_objectives.push_back(objective);
        m_objective_consts.push_back(r);
        m_objective_assignments.push_back(vr);
    }
    else {
        result = null_theory_var;
    }
    return result; 
}

template<typename Ext>
expr_ref theory_diff_logic_weak<Ext>::mk_ineq(theory_var v, inf_eps const& val, bool is_strict) {
    IF_VERBOSE(5, verbose_stream() << "W-DL: mk_ineq: " << v << "\n";);
    objective_term const& t = m_objectives[v];
    expr_ref e(m), f(m), f2(m);
    if (t.size() == 1 && t[0].second.is_one()) {
        f = get_enode(t[0].first)->get_owner();
    }
    else if (t.size() == 1 && t[0].second.is_minus_one()) {
        f = m_util.mk_uminus(get_enode(t[0].first)->get_owner());
    }
    else if (t.size() == 2 && t[0].second.is_one() && t[1].second.is_minus_one()) {
        f = get_enode(t[0].first)->get_owner();
        f2 = get_enode(t[1].first)->get_owner();
        f = m_util.mk_sub(f, f2); 
    }
    else if (t.size() == 2 && t[1].second.is_one() && t[0].second.is_minus_one()) {
        f = get_enode(t[1].first)->get_owner();
        f2 = get_enode(t[0].first)->get_owner();
        f = m_util.mk_sub(f, f2);
    }
    else {
        // 
        expr_ref_vector const& core = m_objective_assignments[v];
        f = m.mk_and(core.size(), core.c_ptr());
        if (is_strict) {
            f = m.mk_not(f);
        }
        return f;
    }

    inf_eps new_val = val; // - inf_rational(m_objective_consts[v]);
    e = m_util.mk_numeral(new_val.get_rational(), m.get_sort(f));
    
    if (new_val.get_infinitesimal().is_neg()) {
        if (is_strict) {
            f = m_util.mk_ge(f, e);
        }
        else {
            expr_ref_vector const& core = m_objective_assignments[v];
            f = m.mk_and(core.size(), core.c_ptr());            
        }
    }
    else {
        if (is_strict) {
            f = m_util.mk_gt(f, e);
        }
        else {
            f = m_util.mk_ge(f, e);
        }
    }
    return f;
}

template<typename Ext>
expr_ref theory_diff_logic_weak<Ext>::mk_gt(theory_var v, inf_eps const& val) {
    IF_VERBOSE(5, verbose_stream() << "W-DL: mk_gt: " << v << "\n";);
    return mk_ineq(v, val, true);
}

template<typename Ext>
expr_ref theory_diff_logic_weak<Ext>::mk_ge(generic_model_converter& fm, theory_var v, inf_eps const& val) {
    IF_VERBOSE(5, verbose_stream() << "W-DL: mk_ge: " << v << "\n";);
    return mk_ineq(v, val, false);
}

#if 0
    model_ref mdl;
    ctx.get_model(mdl);
    ptr_vector<expr> formulas(ctx.get_num_asserted_formulas(), ctx.get_asserted_formulas());
    model_implicant impl_extractor(m);
    expr_ref_vector implicants = impl_extractor.minimize_literals(formulas, mdl);
    return m.mk_and(o, m.mk_not(m.mk_and(implicants.size(), implicants.c_ptr())));
#endif

template<typename Ext>
bool theory_diff_logic_weak<Ext>::internalize_objective(expr * n, rational const& m, rational& q, objective_term & objective) {
    IF_VERBOSE(15, verbose_stream() << "W-DL: internalize_objective\n" << mk_pp(n, get_manager()) << "\n";);

    // Compile term into objective_term format
    rational r;
    expr* x, *y;
    if (m_util.is_numeral(n, r)) {
        q += r;
    }
    else if (m_util.is_add(n)) {
        for (unsigned i = 0; i < to_app(n)->get_num_args(); ++i) {
            if (!internalize_objective(to_app(n)->get_arg(i), m, q, objective)) {
                return false;
            }
        }
    }
    else if (m_util.is_mul(n, x, y) && m_util.is_numeral(x, r)) {
        return internalize_objective(y, m*r, q, objective);
    }
    else if (m_util.is_mul(n, y, x) && m_util.is_numeral(x, r)) {
        return internalize_objective(y, m*r, q, objective);
    }
    else if (!is_app(n)) {
        return false;
    }
    else if (to_app(n)->get_family_id() == m_util.get_family_id()) {
        return false;
    }
    else {
        theory_var v = mk_var(to_app(n));
        objective.push_back(std::make_pair(v, m));
    }
    return true;
}

template<typename Ext>
theory* theory_diff_logic_weak<Ext>::mk_fresh(context* new_ctx) {
    return alloc(theory_diff_logic_weak<Ext>, *new_ctx);
}

template<typename Ext>
void theory_diff_logic_weak<Ext>::init_zero() {
    if (m_izero != null_theory_var) return;
    TRACE("arith", tout << "init zero\n";);
    app* zero;
    enode* e;
    zero = m_util.mk_numeral(rational(0), true);
    e = ctx.mk_enode(zero, false, false, true);
    SASSERT(!is_attached_to_var(e));
    m_izero = mk_var(e);

    zero = m_util.mk_numeral(rational(0), false);
    e = ctx.mk_enode(zero, false, false, true);
    SASSERT(!is_attached_to_var(e));
    m_rzero = mk_var(e);   
}


