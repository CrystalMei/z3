/*++
Copyright (c) 2008 Microsoft Corporation

Module Name:

    theory_diff_logic_weak.cpp

Abstract:

    <abstract>

Author:

    Leonardo de Moura (leonardo) 2008-04-21.
    Nikolaj Bjorner (nbjorner) 2008-05-05

Revision History:

--*/
#include "smt/theory_diff_logic_weak.h"

#include "util/rational.h"
#include "smt/theory_diff_logic_weak_def.h"
#include "math/simplex/sparse_matrix_def.h"

namespace smt {

template class theory_diff_logic_weak<weak_idl_ext>;
template class theory_diff_logic_weak<weak_sidl_ext>;
template class theory_diff_logic_weak<weak_rdl_ext>;
template class theory_diff_logic_weak<weak_srdl_ext>;


};

namespace simplex {
template class simplex<mpq_ext>;
template class sparse_matrix<mpq_ext>;
};
