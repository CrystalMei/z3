/*++
Copyright (c) 2012 Microsoft Corporation

Module Name:

    dla_tactic.h

Abstract:

    General-like tactic for DLA logic

Author:

    XXX

Notes:

--*/
#pragma once

#include "util/params.h"
class ast_manager;
class tactic;

tactic * mk_dla_tactic(ast_manager & m, params_ref const & p = params_ref());

/*
ADD_TACTIC("dla", "builtin strategy for solving DLA problems", "mk_dla_tactic(m, p)")
*/

