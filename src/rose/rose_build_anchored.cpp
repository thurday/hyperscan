/*
 * Copyright (c) 2015, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Intel Corporation nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "rose_build_anchored.h"

#include "grey.h"
#include "rose_build_impl.h"
#include "rose_internal.h"
#include "ue2common.h"
#include "nfa/dfa_min.h"
#include "nfa/mcclellancompile.h"
#include "nfa/mcclellancompile_util.h"
#include "nfa/nfa_build_util.h"
#include "nfa/rdfa_merge.h"
#include "nfagraph/ng_holder.h"
#include "nfagraph/ng_repeat.h"
#include "nfagraph/ng_util.h"
#include "nfagraph/ng_mcclellan_internal.h"
#include "util/alloc.h"
#include "util/bitfield.h"
#include "util/charreach.h"
#include "util/compile_context.h"
#include "util/compile_error.h"
#include "util/container.h"
#include "util/determinise.h"
#include "util/graph_range.h"
#include "util/make_unique.h"
#include "util/order_check.h"
#include "util/ue2_containers.h"
#include "util/ue2string.h"
#include "util/verify_types.h"

#include <map>
#include <queue>
#include <set>
#include <vector>

using namespace std;

namespace ue2 {

#define ANCHORED_NFA_STATE_LIMIT 512
#define MAX_DFA_STATES           16000
#define DFA_PAIR_MERGE_THRESHOLD 5000
#define MAX_SMALL_START_REACH    4

#define INIT_STATE (DEAD_STATE + 1)

// Adds a vertex with the given reach.
static
NFAVertex add_vertex(NGHolder &h, const CharReach &cr) {
    NFAVertex v = add_vertex(h);
    h[v].char_reach = cr;
    return v;
}

static
void add_edges(const set<NFAVertex> &parents, NFAVertex v, NGHolder &h) {
    for (auto p : parents) {
        add_edge(p, v, h);
    }
}

static
set<NFAVertex> addDotsToGraph(NGHolder &h, NFAVertex start, u32 min, u32 max,
                              const CharReach &cr) {
    DEBUG_PRINTF("adding [%u, %u] to graph\n", min, max);
    u32 i = 0;
    set<NFAVertex> curr;
    curr.insert(start);
    for (; i < min; i++) {
        NFAVertex next = add_vertex(h, cr);
        add_edges(curr, next, h);
        curr.clear();
        curr.insert(next);
    }

    assert(max != ROSE_BOUND_INF);

    set<NFAVertex> orig = curr;
    for (; i < max; i++) {
        NFAVertex next = add_vertex(h, cr);
        add_edges(curr, next, h);
        curr.clear();
        curr.insert(next);
        curr.insert(orig.begin(), orig.end());
    }

    return curr;
}

static
NFAVertex addToGraph(NGHolder &h, const set<NFAVertex> &curr,
                     const ue2_literal &s) {
    DEBUG_PRINTF("adding %s to graph\n", dumpString(s).c_str());
    assert(!s.empty());

    ue2_literal::const_iterator it = s.begin();
    NFAVertex u = add_vertex(h, *it);
    add_edges(curr, u, h);

    for (++it; it != s.end(); ++it) {
        NFAVertex next = add_vertex(h, *it);
        add_edge(u, next, h);
        u = next;
    }

    return u;
}

static
void mergeAnchoredDfas(vector<unique_ptr<raw_dfa>> &dfas,
                       const RoseBuildImpl &build) {
    // First, group our DFAs into "small start" and "big start" sets.
    vector<unique_ptr<raw_dfa>> small_starts, big_starts;
    for (auto &rdfa : dfas) {
        u32 start_size = mcclellanStartReachSize(rdfa.get());
        if (start_size <= MAX_SMALL_START_REACH) {
            small_starts.push_back(move(rdfa));
        } else {
            big_starts.push_back(move(rdfa));
        }
    }
    dfas.clear();

    DEBUG_PRINTF("%zu dfas with small starts, %zu dfas with big starts\n",
                  small_starts.size(), big_starts.size());
    mergeDfas(small_starts, MAX_DFA_STATES, nullptr, build.cc.grey);
    mergeDfas(big_starts, MAX_DFA_STATES, nullptr, build.cc.grey);

    // Rehome our groups into one vector.
    for (auto &rdfa : small_starts) {
        dfas.push_back(move(rdfa));
    }
    for (auto &rdfa : big_starts) {
        dfas.push_back(move(rdfa));
    }

    // Final test: if we've built two DFAs here that are small enough, we can
    // try to merge them.
    if (dfas.size() == 2) {
        size_t total_states = dfas[0]->states.size() + dfas[1]->states.size();
        if (total_states < DFA_PAIR_MERGE_THRESHOLD) {
            DEBUG_PRINTF("doing small pair merge\n");
            mergeDfas(dfas, MAX_DFA_STATES, nullptr, build.cc.grey);
        }
    }
}

static
void translateReportSet(flat_set<ReportID> *rset, const RoseBuildImpl &tbi) {
    flat_set<ReportID> old;
    old.swap(*rset);
    for (auto report_id : old) {
        DEBUG_PRINTF("updating %u -> %u\n", report_id,
                     tbi.literal_info[report_id].final_id);
        rset->insert(tbi.literal_info[report_id].final_id);
    }
}

static
void remapAnchoredReports(raw_dfa &dfa, const RoseBuildImpl &tbi) {
    for (dstate &ds : dfa.states) {
        translateReportSet(&ds.reports, tbi);
        translateReportSet(&ds.reports_eod, tbi);
    }
}

/* Replaces the report ids currently in the dfas (rose graph literal ids) with
 * the final id used by the runtime. */
static
void remapAnchoredReports(RoseBuildImpl &tbi) {
    for (auto it = tbi.anchored_nfas.begin(); it != tbi.anchored_nfas.end();
         ++it) {
        for (auto &rdfa : it->second) {
            assert(rdfa);
            remapAnchoredReports(*rdfa, tbi);
        }
    }
}

static
void populate_holder(const simple_anchored_info &sai, const set<u32> &exit_ids,
                     NGHolder *h_in) {
    DEBUG_PRINTF("populating holder for ^.{%u,%u}%s\n", sai.min_bound,
                 sai.max_bound, dumpString(sai.literal).c_str());
    NGHolder &h = *h_in;
    set<NFAVertex> ends = addDotsToGraph(h, h.start, sai.min_bound,
                                         sai.max_bound, CharReach::dot());
    NFAVertex v = addToGraph(h, ends, sai.literal);
    add_edge(v, h.accept, h);
    h[v].reports.insert(exit_ids.begin(), exit_ids.end());
}

u32 anchoredStateSize(const void *atable) {
    if (!atable) {
        return 0;
    }

    const struct anchored_matcher_info *curr
        = (const anchored_matcher_info *)atable;

    // Walk the list until we find the last element; total state size will be
    // that engine's state offset plus its state requirement.
    while (curr->next_offset) {
        curr = (const anchored_matcher_info *)
            ((const char *)curr + curr->next_offset);
    }

    const NFA *nfa = (const NFA *)((const char *)curr + sizeof(*curr));
    return curr->state_offset + nfa->scratchStateSize;
}

bool anchoredIsMulti(const RoseEngine &engine) {
    const struct anchored_matcher_info *curr
        = (const anchored_matcher_info *)getALiteralMatcher(&engine);

    return curr && curr->next_offset;
 }


namespace {

typedef bitfield<ANCHORED_NFA_STATE_LIMIT> nfa_state_set;

struct Holder_StateSet {
    Holder_StateSet() : wdelay(0) {}

    nfa_state_set wrap_state;
    u32 wdelay;

    bool operator==(const Holder_StateSet &b) const {
        return wdelay == b.wdelay && wrap_state == b.wrap_state;
    }
};

size_t hash_value(const Holder_StateSet &s) {
    size_t val = 0;
    boost::hash_combine(val, s.wrap_state);
    boost::hash_combine(val, s.wdelay);
    return val;
}

class Automaton_Holder {
public:
    typedef Holder_StateSet StateSet;
    typedef ue2::unordered_map<StateSet, dstate_id_t> StateMap;

    explicit Automaton_Holder(const NGHolder &g_in) : g(g_in), bad(false) {
        for (auto v : vertices_range(g)) {
            vertexToIndex[v] = indexToVertex.size();
            indexToVertex.push_back(v);
        }

        if (indexToVertex.size() > ANCHORED_NFA_STATE_LIMIT) {
            bad = true;
            return;
        }

        DEBUG_PRINTF("%zu states\n", indexToVertex.size());
        init.wdelay = 0;
        init.wrap_state.set(vertexToIndex[g.start]);

        DEBUG_PRINTF("init wdelay %u\n", init.wdelay);

        calculateAlphabet();
        cr_by_index = populateCR(g, indexToVertex, alpha);
    }

private:
    void calculateAlphabet() {
        vector<CharReach> esets(1, CharReach::dot());

        for (auto v : indexToVertex) {
            const CharReach &cr = g[v].char_reach;

            for (size_t i = 0; i < esets.size(); i++) {
                if (esets[i].count() == 1) {
                    continue;
                }

                CharReach t = cr & esets[i];

                if (t.any() && t != esets[i]) {
                    esets[i] &= ~t;
                    esets.push_back(t);
                }
            }
        }

        alphasize = buildAlphabetFromEquivSets(esets, alpha, unalpha);
    }

public:
    void transition(const StateSet &in, StateSet *next) {
        /* track the dfa state, reset nfa states */
        u32 wdelay = in.wdelay ? in.wdelay - 1 : 0;

        for (symbol_t s = 0; s < alphasize; s++) {
            next[s].wrap_state.reset();
            next[s].wdelay = wdelay;
        }

        nfa_state_set succ;

        if (wdelay != in.wdelay) {
            DEBUG_PRINTF("enabling start\n");
            succ.set(vertexToIndex[g.startDs]);
        }

        for (size_t i = in.wrap_state.find_first(); i != nfa_state_set::npos;
             i = in.wrap_state.find_next(i)) {
            NFAVertex v = indexToVertex[i];
            for (auto w : adjacent_vertices_range(v, g)) {
                if (!contains(vertexToIndex, w)
                    || w == g.accept || w == g.acceptEod) {
                    continue;
                }

                if (w == g.startDs) {
                    continue;
                }

                succ.set(vertexToIndex[w]);
            }
        }

        for (size_t j = succ.find_first(); j != nfa_state_set::npos;
             j = succ.find_next(j)) {
            const CharReach &cr = cr_by_index[j];
            for (size_t s = cr.find_first(); s != CharReach::npos;
                 s = cr.find_next(s)) {
                next[s].wrap_state.set(j); /* pre alpha'ed */
            }
        }

        next[alpha[TOP]] = in;
    }

    const vector<StateSet> initial() {
        return {init};
    }

    void reports(const StateSet &in, flat_set<ReportID> &rv) {
        rv.clear();
        for (size_t i = in.wrap_state.find_first(); i != nfa_state_set::npos;
             i = in.wrap_state.find_next(i)) {
            NFAVertex v = indexToVertex[i];
            if (edge(v, g.accept, g).second) {
                assert(!g[v].reports.empty());
                insert(&rv, g[v].reports);
            } else {
                assert(g[v].reports.empty());
            }
        }
    }

    void reportsEod(const StateSet &, flat_set<ReportID> &r) {
        r.clear();
    }

    static bool canPrune(const flat_set<ReportID> &) {
        /* used by ng_ to prune states after highlander accepts */
        return false;
    }

private:
    const NGHolder &g;
    ue2::unordered_map<NFAVertex, u32> vertexToIndex;
    vector<NFAVertex> indexToVertex;
    vector<CharReach> cr_by_index;
    StateSet init;
public:
    StateSet dead;
    array<u16, ALPHABET_SIZE> alpha;
    array<u16, ALPHABET_SIZE> unalpha;
    u16 alphasize;
    bool bad;
};

} // namespace

static
bool check_dupe(const raw_dfa &rdfa,
                const vector<unique_ptr<raw_dfa>> &existing, ReportID *remap) {
    if (!remap) {
        DEBUG_PRINTF("no remap\n");
        return false;
    }

    set<ReportID> rdfa_reports;
    for (const auto &ds : rdfa.states) {
        rdfa_reports.insert(ds.reports.begin(), ds.reports.end());
    }
    if (rdfa_reports.size() != 1) {
        return false; /* too complicated for now would need mapping TODO */
    }

    for (const auto &e_rdfa : existing) {
        assert(e_rdfa);
        const raw_dfa &b = *e_rdfa;

        if (rdfa.start_anchored != b.start_anchored ||
            rdfa.alpha_size != b.alpha_size ||
            rdfa.states.size() != b.states.size() ||
            rdfa.alpha_remap != b.alpha_remap) {
            continue;
        }

        set<ReportID> b_reports;

        for (u32 i = 0; i < b.states.size(); i++) {
            assert(b.states[i].reports_eod.empty());
            assert(rdfa.states[i].reports_eod.empty());
            if (rdfa.states[i].reports.size() != b.states[i].reports.size()) {
                goto next_dfa;
            }
            b_reports.insert(b.states[i].reports.begin(),
                             b.states[i].reports.end());

            assert(rdfa.states[i].next.size() == b.states[i].next.size());
            if (!equal(rdfa.states[i].next.begin(), rdfa.states[i].next.end(),
                       b.states[i].next.begin())) {
                goto next_dfa;
            }
        }

        if (b_reports.size() != 1) {
            continue;
        }

        *remap = *b_reports.begin();
        DEBUG_PRINTF("dupe found remapping to %u\n", *remap);
        return true;
    next_dfa:;
    }

    return false;
}

static
bool check_dupe_simple(const RoseBuildImpl &tbi, u32 min_bound, u32 max_bound,
                       const ue2_literal &lit, ReportID *remap) {
    if (!remap) {
        DEBUG_PRINTF("no remap\n");
        return false;
    }

    simple_anchored_info sai(min_bound, max_bound, lit);
    if (contains(tbi.anchored_simple, sai)) {
        *remap = *tbi.anchored_simple.at(sai).begin();
        return true;
    }

    return false;
}

static
NFAVertex extractLiteral(const NGHolder &h, ue2_literal *lit) {
    vector<NFAVertex> lit_verts;
    NFAVertex v = h.accept;
    while ((v = getSoleSourceVertex(h, v))) {
        const CharReach &cr = h[v].char_reach;
        if (cr.count() > 1 && !cr.isCaselessChar()) {
            break;
        }
        lit_verts.push_back(v);
    }

    if (lit_verts.empty()) {
        return NFAGraph::null_vertex();
    }

    bool nocase = false;
    bool case_set = false;

    for (auto it = lit_verts.rbegin(), ite = lit_verts.rend(); it != ite;
         ++it) {
        const CharReach &cr = h[*it].char_reach;
        if (cr.isAlpha()) {
            bool cr_nocase = cr.count() != 1;
            if (case_set && cr_nocase != nocase) {
                return NFAGraph::null_vertex();
            }

            case_set = true;
            nocase = cr_nocase;
            lit->push_back(cr.find_first(), nocase);
        } else {
            lit->push_back(cr.find_first(), false);
        }
    }

    return lit_verts.back();
}

static
bool isSimple(const NGHolder &h, u32 *min_bound, u32 *max_bound,
              ue2_literal *lit, u32 *report) {
    assert(!proper_out_degree(h.startDs, h));
    assert(in_degree(h.acceptEod, h) == 1);

    DEBUG_PRINTF("looking for simple case\n");
    NFAVertex lit_head = extractLiteral(h, lit);

    if (lit_head == NFAGraph::null_vertex()) {
        DEBUG_PRINTF("no literal found\n");
        return false;
    }

    const auto &reps = h[*inv_adjacent_vertices(h.accept, h).first].reports;

    if (reps.size() != 1) {
        return false;
    }
    *report = *reps.begin();

    assert(!lit->empty());

    set<NFAVertex> rep_exits;

    /* lit should only be connected to dot vertices */
    for (auto u : inv_adjacent_vertices_range(lit_head, h)) {
        DEBUG_PRINTF("checking %u\n", h[u].index);
        if (!h[u].char_reach.all()) {
            return false;
        }

        if (u != h.start) {
            rep_exits.insert(u);
        }
    }

    if (rep_exits.empty()) {
        DEBUG_PRINTF("direct anchored\n");
        assert(edge(h.start, lit_head, h).second);
        *min_bound = 0;
        *max_bound = 0;
        return true;
    }

    NFAVertex key = *rep_exits.begin();

    // Special-case the check for '^.foo' or '^.?foo'.
    if (rep_exits.size() == 1 && edge(h.start, key, h).second &&
        out_degree(key, h) == 1) {
        DEBUG_PRINTF("one exit\n");
        assert(edge(h.start, h.startDs, h).second);
        size_t num_enters = out_degree(h.start, h);
        if (num_enters == 2) {
            DEBUG_PRINTF("^.{1,1} prefix\n");
            *min_bound = 1;
            *max_bound = 1;
            return true;
        }
        if (num_enters == 3 && edge(h.start, lit_head, h).second) {
            DEBUG_PRINTF("^.{0,1} prefix\n");
            *min_bound = 0;
            *max_bound = 1;
            return true;
        }
    }

    vector<GraphRepeatInfo> repeats;
    findRepeats(h, 2, &repeats);

    vector<GraphRepeatInfo>::const_iterator it;
    for (it = repeats.begin(); it != repeats.end(); ++it) {
        DEBUG_PRINTF("checking.. %zu verts\n", it->vertices.size());
        if (find(it->vertices.begin(), it->vertices.end(), key)
            != it->vertices.end()) {
            break;
        }
    }
    if (it == repeats.end()) {
        DEBUG_PRINTF("no repeat found\n");
        return false;
    }

    set<NFAVertex> rep_verts;
    insert(&rep_verts, it->vertices);
    if (!is_subset_of(rep_exits, rep_verts)) {
        DEBUG_PRINTF("bad exit check\n");
        return false;
    }

    set<NFAVertex> rep_enters;
    insert(&rep_enters, adjacent_vertices(h.start, h));
    rep_enters.erase(lit_head);
    rep_enters.erase(h.startDs);

    if (!is_subset_of(rep_enters, rep_verts)) {
        DEBUG_PRINTF("bad entry check\n");
        return false;
    }

    u32 min_b = it->repeatMin;
    if (edge(h.start, lit_head, h).second) { /* jump edge */
        if (min_b != 1) {
            DEBUG_PRINTF("jump edge around repeat with min bound\n");
            return false;
        }

        min_b = 0;
    }
    *min_bound = min_b;
    *max_bound = it->repeatMax;

    DEBUG_PRINTF("repeat %u %u before %s\n", *min_bound, *max_bound,
                  dumpString(*lit).c_str());
    return true;
}

static
int finalise_out(RoseBuildImpl &tbi, const NGHolder &h,
                 const Automaton_Holder &autom, unique_ptr<raw_dfa> out_dfa,
                 ReportID *remap) {
    u32 min_bound = ~0U;
    u32 max_bound = ~0U;
    ue2_literal lit;
    u32 simple_report = MO_INVALID_IDX;
    if (isSimple(h, &min_bound, &max_bound, &lit, &simple_report)) {
        assert(simple_report != MO_INVALID_IDX);
        if (check_dupe_simple(tbi, min_bound, max_bound, lit, remap)) {
            DEBUG_PRINTF("found duplicate remapping to %u\n", *remap);
            return ANCHORED_REMAP;
        }
        DEBUG_PRINTF("add with report %u\n", simple_report);
        tbi.anchored_simple[simple_anchored_info(min_bound, max_bound, lit)]
            .insert(simple_report);
        return ANCHORED_SUCCESS;
    }

    out_dfa->start_anchored = INIT_STATE;
    out_dfa->start_floating = DEAD_STATE;
    out_dfa->alpha_size = autom.alphasize;
    out_dfa->alpha_remap = autom.alpha;
    auto hash = hash_dfa_no_reports(*out_dfa);
    if (check_dupe(*out_dfa, tbi.anchored_nfas[hash], remap)) {
        return ANCHORED_REMAP;
    }
    tbi.anchored_nfas[hash].push_back(move(out_dfa));
    return ANCHORED_SUCCESS;
}

static
int addAutomaton(RoseBuildImpl &tbi, const NGHolder &h, ReportID *remap) {
    Automaton_Holder autom(h);

    if (autom.bad) {
        DEBUG_PRINTF("autom bad!\n");
        return ANCHORED_FAIL;
    }

    unique_ptr<raw_dfa> out_dfa = ue2::make_unique<raw_dfa>(NFA_OUTFIX);
    if (!determinise(autom, out_dfa->states, MAX_DFA_STATES)) {
        return finalise_out(tbi, h, autom, move(out_dfa), remap);
    }

    DEBUG_PRINTF("determinise failed\n");
    return ANCHORED_FAIL;
}

static
void setReports(NGHolder &h, const map<NFAVertex, set<u32>> &reportMap,
                const ue2::unordered_map<NFAVertex, NFAVertex> &orig_to_copy) {
    for (const auto &m : reportMap) {
        NFAVertex t = orig_to_copy.at(m.first);
        assert(!m.second.empty());
        add_edge(t, h.accept, h);
        insert(&h[t].reports, m.second);
    }
}

int addAnchoredNFA(RoseBuildImpl &tbi, const NGHolder &wrapper,
                   const map<NFAVertex, set<u32>> &reportMap) {
    NGHolder h;
    ue2::unordered_map<NFAVertex, NFAVertex> orig_to_copy;
    cloneHolder(h, wrapper, &orig_to_copy);
    clear_in_edges(h.accept, h);
    clear_in_edges(h.acceptEod, h);
    add_edge(h.accept, h.acceptEod, h);
    clearReports(h);
    setReports(h, reportMap, orig_to_copy);

    return addAutomaton(tbi, h, nullptr);
}

int addToAnchoredMatcher(RoseBuildImpl &tbi, const NGHolder &anchored,
                         u32 exit_id, ReportID *remap) {
    NGHolder h;
    cloneHolder(h, anchored);
    clearReports(h);
    assert(in_degree(h.acceptEod, h) == 1);
    for (auto v : inv_adjacent_vertices_range(h.accept, h)) {
        h[v].reports.clear();
        h[v].reports.insert(exit_id);
    }

    return addAutomaton(tbi, h, remap);
}

static
void buildSimpleDfas(const RoseBuildImpl &tbi,
                     vector<unique_ptr<raw_dfa>> *anchored_dfas) {
    /* we should have determinised all of these before so there should be no
     * chance of failure. */
     for (const auto &simple : tbi.anchored_simple) {
        set<u32> exit_ids;
        for (auto lit_id : simple.second) {
            exit_ids.insert(tbi.literal_info[lit_id].final_id);
        }
        NGHolder h;
        populate_holder(simple.first, exit_ids, &h);
        Automaton_Holder autom(h);
        assert(!autom.bad);
        unique_ptr<raw_dfa> rdfa = ue2::make_unique<raw_dfa>(NFA_OUTFIX);
        UNUSED int rv = determinise(autom, rdfa->states, MAX_DFA_STATES);
        assert(!rv);
        rdfa->start_anchored = INIT_STATE;
        rdfa->start_floating = DEAD_STATE;
        rdfa->alpha_size = autom.alphasize;
        rdfa->alpha_remap = autom.alpha;
        anchored_dfas->push_back(move(rdfa));
    }
}

/**
 * Fill the given vector with all of the raw_dfas we need to compile into the
 * anchored matcher. Takes ownership of the input structures, clearing them
 * from RoseBuildImpl.
 */
static
void getAnchoredDfas(RoseBuildImpl &tbi,
                     vector<unique_ptr<raw_dfa>> *anchored_dfas) {
    // DFAs that already exist as raw_dfas.
    for (auto &anch_dfas : tbi.anchored_nfas) {
        for (auto &rdfa : anch_dfas.second) {
            anchored_dfas->push_back(move(rdfa));
        }
    }
    tbi.anchored_nfas.clear();

    // DFAs we currently have as simple literals.
    if (!tbi.anchored_simple.empty()) {
        buildSimpleDfas(tbi, anchored_dfas);
        tbi.anchored_simple.clear();
    }
}

/**
 * \brief Builds our anchored DFAs into runtime NFAs.
 *
 * Constructs a vector of NFA structures and a vector of their start offsets
 * (number of dots removed from the prefix) from the raw_dfa structures given.
 *
 * Note: frees the raw_dfa structures on completion.
 *
 * \return Total bytes required for the complete anchored matcher.
 */
static
size_t buildNfas(vector<unique_ptr<raw_dfa>> &anchored_dfas,
                 vector<aligned_unique_ptr<NFA>> *nfas, vector<u32> *start_offset,
                 const CompileContext &cc) {
    const size_t num_dfas = anchored_dfas.size();

    nfas->reserve(num_dfas);
    start_offset->reserve(num_dfas);

    size_t total_size = 0;

    for (auto &rdfa : anchored_dfas) {
        u32 removed_dots = remove_leading_dots(*rdfa);
        start_offset->push_back(removed_dots);

        minimize_hopcroft(*rdfa, cc.grey);

        aligned_unique_ptr<NFA> nfa = mcclellanCompile(*rdfa, cc);
        if (!nfa) {
            assert(0);
            throw std::bad_alloc();
        }

        assert(nfa->length);
        total_size += ROUNDUP_CL(sizeof(anchored_matcher_info) + nfa->length);
        nfas->push_back(move(nfa));
    }

    // We no longer need to keep the raw_dfa structures around.
    anchored_dfas.clear();

    return total_size;
}

aligned_unique_ptr<void> buildAnchoredAutomataMatcher(RoseBuildImpl &tbi,
                                                      size_t *asize) {
    const CompileContext &cc = tbi.cc;
    remapAnchoredReports(tbi);

    if (tbi.anchored_nfas.empty() && tbi.anchored_simple.empty()) {
        DEBUG_PRINTF("empty\n");
        *asize = 0;
        return nullptr;
    }

    vector<unique_ptr<raw_dfa>> anchored_dfas;
    getAnchoredDfas(tbi, &anchored_dfas);

    mergeAnchoredDfas(anchored_dfas, tbi);

    vector<aligned_unique_ptr<NFA>> nfas;
    vector<u32> start_offset; // start offset for each dfa (dots removed)
    size_t total_size = buildNfas(anchored_dfas, &nfas, &start_offset, cc);

    if (total_size > cc.grey.limitRoseAnchoredSize) {
        throw ResourceLimitError();
    }

    *asize = total_size;
    aligned_unique_ptr<void> atable = aligned_zmalloc_unique<void>(total_size);
    char *curr = (char *)atable.get();

    u32 state_offset = 0;
    for (size_t i = 0; i < nfas.size(); i++) {
        const NFA *nfa = nfas[i].get();
        anchored_matcher_info *ami = (anchored_matcher_info *)curr;
        char *prev_curr = curr;

        curr += sizeof(anchored_matcher_info);

        memcpy(curr, nfa, nfa->length);
        curr += nfa->length;
        curr = ROUNDUP_PTR(curr, 64);

        if (i + 1 == nfas.size()) {
            ami->next_offset = 0U;
        } else {
            ami->next_offset = verify_u32(curr - prev_curr);
        }

        // State must be aligned.
        u32 align_req = state_alignment(*nfa);
        assert(align_req <= 2); // only DFAs.
        while (state_offset % align_req) {
            state_offset++;
        }

        ami->state_offset = state_offset;
        state_offset += nfa->scratchStateSize;
        ami->anchoredMinDistance = start_offset[i];
    }

    DEBUG_PRINTF("success %zu\n", *asize);
    return atable;
}

} // namespace ue2
