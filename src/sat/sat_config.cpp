/*++
Copyright (c) 2011 Microsoft Corporation

Module Name:

    sat_config.cpp

Abstract:

    SAT configuration options

Author:

    Leonardo de Moura (leonardo) 2011-05-21.

Revision History:

--*/
#include"sat_config.h"
#include"sat_types.h"

namespace sat {

    config::config(params_ref const & p):
        m_always_true("always_true"),
        m_always_false("always_false"),
        m_caching("caching"),
        m_random("random"),
        m_geometric("geometric"),
        m_luby("luby"),
        m_dyn_psm("dyn_psm"),
        m_psm("psm"),
        m_glue("glue"),
        m_glue_psm("glue_psm"),
        m_psm_glue("psm_glue") {
        updt_params(p); 
    }

    void config::updt_params(params_ref const & p) {
        m_max_memory  = megabytes_to_bytes(p.get_uint("max_memory", UINT_MAX));

        symbol s = p.get_sym("restart", m_luby);
        if (s == m_luby)
            m_restart = RS_LUBY;
        else if (s == m_geometric)
            m_restart = RS_GEOMETRIC;
        else
            throw sat_param_exception("invalid restart strategy");

        s = p.get_sym("phase", m_caching);
        if (s == m_always_false) 
            m_phase = PS_ALWAYS_FALSE;
        else if (s == m_always_true)
            m_phase = PS_ALWAYS_TRUE;
        else if (s == m_caching)
            m_phase = PS_CACHING;
        else if (s == m_random)
            m_phase = PS_RANDOM;
        else
            throw sat_param_exception("invalid phase selection strategy");

        m_phase_caching_on  = p.get_uint("phase_caching_on", 400);
        m_phase_caching_off = p.get_uint("phase_caching_off", 100);

        m_restart_initial = p.get_uint("restart_initial", 100);
        m_restart_factor  = p.get_double("restart_factor", 1.5);
        
        m_random_freq     = p.get_double("random_freq", 0.01);

        m_burst_search    = p.get_uint("burst_search", 100);
        
        m_max_conflicts   = p.get_uint("max_conflicts", UINT_MAX);

        m_simplify_mult1  = p.get_uint("simplify_mult1", 300);
        m_simplify_mult2  = p.get_double("simplify_mult2", 1.5);
        m_simplify_max    = p.get_uint("simplify_max", 500000);

        s = p.get_sym("gc_strategy", m_glue_psm);
        if (s == m_dyn_psm) {
            m_gc_strategy     = GC_DYN_PSM;
            m_gc_initial      = p.get_uint("gc_initial", 500);
            m_gc_increment    = p.get_uint("gc_increment", 100);
            m_gc_small_lbd    = p.get_uint("gc_small_lbd", 3);
            m_gc_k            = p.get_uint("gc_k", 7);
            if (m_gc_k > 255)
                m_gc_k = 255;
        }
        else {
            if (s == m_glue_psm)
                m_gc_strategy = GC_GLUE_PSM;
            else if (s == m_glue)
                m_gc_strategy = GC_GLUE;
            else if (s == m_psm)
                m_gc_strategy = GC_PSM;
            else if (s == m_psm_glue)
                m_gc_strategy = GC_PSM_GLUE;
            else 
                throw sat_param_exception("invalid gc strategy");
            m_gc_initial      = p.get_uint("gc_initial", 20000);
            m_gc_increment    = p.get_uint("gc_increment", 500);
        }
        m_minimize_lemmas = p.get_bool("minimize_lemmas", true);
        m_dyn_sub_res     = p.get_bool("dyn_sub_res", true);
    }

    void config::collect_param_descrs(param_descrs & r) {
        insert_max_memory(r);
        r.insert("phase", CPK_SYMBOL, "(default: caching) phase selection strategy: always_false, always_true, caching, random.");
        r.insert("phase_caching_on", CPK_UINT, "(default: 400)");
        r.insert("phase_caching_off", CPK_UINT, "(default: 100)");
        r.insert("restart", CPK_SYMBOL, "(default: luby) restart strategy: luby or geometric.");
        r.insert("restart_initial", CPK_UINT, "(default: 100) initial restart (number of conflicts).");  
        r.insert("restart_factor", CPK_DOUBLE, "(default: 1.5) restart increment factor for geometric strategy.");
        r.insert("random_freq", CPK_DOUBLE, "(default: 0.01) frequency of random case splits.");                                                      
        r.insert("burst_search", CPK_UINT, "(default: 100) number of conflicts before first global simplification.");
        r.insert("max_conflicts", CPK_UINT, "(default: inf) maximum number of conflicts.");
        r.insert("gc_strategy", CPK_SYMBOL, "(default: glue_psm) garbage collection strategy: psm, glue, glue_psm, dyn_psm.");
        r.insert("gc_initial", CPK_UINT, "(default: 20000) learned clauses garbage collection frequence.");
        r.insert("gc_increment", CPK_UINT, "(default: 500) increment to the garbage collection threshold.");
        r.insert("gc_small_lbd", CPK_UINT, "(default: 3) learned clauses with small LBD are never deleted (only used in dyn_psm).");
        r.insert("gc_k", CPK_UINT, "(default: 7) learned clauses that are inactive for k gc rounds are permanently deleted (only used in dyn_psm).");
        r.insert("minimize_lemmas", CPK_BOOL, "(default: true) minimize learned clauses.");
        r.insert("dyn_sub_res", CPK_BOOL, "(default: true) dynamic subsumption resolution for minimizing learned clauses.");
    }

};
