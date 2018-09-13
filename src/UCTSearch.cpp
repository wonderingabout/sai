/*
    This file is part of Leela Zero.
    Copyright (C) 2017-2018 Gian-Carlo Pascutto and contributors

    Leela Zero is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Leela Zero is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Leela Zero.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "config.h"
#include "UCTSearch.h"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <type_traits>
#include <tuple>

#include "FastBoard.h"
#include "FastState.h"
#include "FullBoard.h"
#include "GTP.h"
#include "GameState.h"
#include "TimeControl.h"
#include "Timing.h"
#include "Training.h"
#include "Utils.h"
#include "Network.h"

using namespace Utils;

constexpr int UCTSearch::UNLIMITED_PLAYOUTS;

UCTSearch::UCTSearch(GameState& g)
    : m_rootstate(g) {
    set_playout_limit(cfg_max_playouts);
    set_visit_limit(cfg_max_visits);
    m_root = std::make_unique<UCTNode>(FastBoard::PASS, 0.0f);
}

bool UCTSearch::advance_to_new_rootstate() {
    if (!m_root || !m_last_rootstate) {
        // No current state
        return false;
    }

    if (m_rootstate.get_komi() != m_last_rootstate->get_komi()) {
        return false;
    }

    auto depth =
        int(m_rootstate.get_movenum() - m_last_rootstate->get_movenum());
    myprintf("Advance to new rootstate. Depth=%i.\n", depth);
    
    if (depth < 0) {
        return false;
    }


    auto test = std::make_unique<GameState>(m_rootstate);
    for (auto i = 0; i < depth; i++) {
        test->undo_move();
    }

    if (m_last_rootstate->board.get_hash() != test->board.get_hash()) {
        // m_rootstate and m_last_rootstate don't match
        return false;
    }

    // Make sure that the nodes we destroyed the previous move are
    // in fact destroyed.
    myprintf("About to destroy nodes: ");
    while (!m_delete_futures.empty()) {
	myprintf("#");
        m_delete_futures.front().wait_all();
        m_delete_futures.pop_front();
    }
    myprintf("\n");

    // Try to replay moves advancing m_root
    myprintf("About to replay moves:");
    for (auto i = 0; i < depth; i++) {
        ThreadGroup tg(thread_pool);

        test->forward_move();
        const auto move = test->get_last_move();

	myprintf(" %i", move);
        auto oldroot = std::move(m_root);
        m_root = oldroot->find_child(move);

        // Lazy tree destruction.  Instead of calling the destructor of the
        // old root node on the main thread, send the old root to a separate
        // thread and destroy it from the child thread.  This will save a
        // bit of time when dealing with large trees.
        auto p = oldroot.release();
        tg.add_task([p]() { delete p; });
        m_delete_futures.push_back(std::move(tg));

        if (!m_root) {
            // Tree hasn't been expanded this far
            return false;
        }
        m_last_rootstate->play_move(move);
    }

    myprintf("\n");
    assert(m_rootstate.get_movenum() == m_last_rootstate->get_movenum());

    if (m_last_rootstate->board.get_hash() != test->board.get_hash()) {
        // Can happen if user plays multiple moves in a row by same player
        return false;
    }

    myprintf("Finihed.");
    return true;
}

void UCTSearch::update_root() {
    // Definition of m_playouts is playouts per search call.
    // So reset this count now.
    m_playouts = 0;

    //#ifndef NDEBUG
    auto start_nodes = m_root->count_nodes();
    myprintf("m_root->count_nodes()=%u.\n", start_nodes);
    //#endif

    if (!advance_to_new_rootstate() || !m_root) {
        m_root = std::make_unique<UCTNode>(FastBoard::PASS, 0.0f);
	myprintf("New m_root created.\n");
    }
    // Clear last_rootstate to prevent accidental use.
    m_last_rootstate.reset(nullptr);

    // Check how big our search tree (reused or new) is.
    int n = m_nodes = m_root->count_nodes();
    myprintf("m_root->count_nodes()=%u.\n", n);

    //#ifndef NDEBUG
    if (m_nodes > 0) {
        myprintf("update_root, %d -> %d nodes (%.1f%% reused)\n",
            start_nodes, m_nodes.load(), 100.0 * m_nodes.load() / start_nodes);
    }
    //#endif
}

float UCTSearch::get_min_psa_ratio() const {
    const auto mem_full = m_nodes / static_cast<float>(MAX_TREE_SIZE);
    // If we are halfway through our memory budget, start trimming
    // moves with very low policy priors.
    if (mem_full > 0.5f) {
        // Memory is almost exhausted, trim more aggressively.
        if (mem_full > 0.95f) {
            return 0.01f;
        }
        return 0.001f;
    }
    return 0.0f;
}

SearchResult UCTSearch::play_simulation(GameState & currstate,
                                        UCTNode* const node) {
    const auto color = currstate.get_to_move();
    auto result = SearchResult{};

    const auto lastmove = currstate.get_last_move();
    const std::string tmp = lastmove ? currstate.move_to_text(lastmove)
	: "empty";
    
    node->virtual_loss();

    myprintf("Last move was %i, or %s. Simulation begins.\n"
	     "Visits=%i, blackevals=%f, eval=%f, net_eval=%f.\n"
	     "Is the node expandable? Default is no.\n",
	     lastmove, tmp.c_str(),
	     node->get_visits(),
	     node->get_blackevals(),
	     node->get_eval(color),
	     node->get_net_eval(color));
    if (node->expandable()) {
	myprintf("Node is expandable.\n");
        if (currstate.get_passes() >= 2) {
            auto score = currstate.final_score();
	    myprintf("Two passes. Score is %f.\n", score);
            result = SearchResult::from_score(score);
        } else if (m_nodes < MAX_TREE_SIZE) {
	    const int n = m_nodes;
	    myprintf("m_nodes=%i < MTS=%i.\n", n, MAX_TREE_SIZE);
	    //            float eval;
	    float value, alpkt, beta;
            const auto had_children = node->has_children();
	    myprintf("has_children() returned %i.\n", had_children);
	    myprintf("About to call create_children(). minpsa_r=%f.\n",
		     get_min_psa_ratio());
            const auto success =
                node->create_children(m_nodes, currstate, value, alpkt, beta,
                                      get_min_psa_ratio());
	    myprintf("Function create_children() returned %i, alpkt=%f, beta=%f.\n",
		     success, alpkt, beta);
	    myprintf("Last move was %i, or %s. Just after create_children().\n"
		     "Visits=%i, blackevals=%f, x_bar=%f, "
		     "eval=%f, net_eval=%f.\n",
		     lastmove, tmp.c_str(),
		     node->get_visits(),
		     node->get_blackevals(),
		     node->get_eval_bonus(),
		     node->get_eval(color),
		     node->get_net_eval(color));
            if (!had_children && success) {
		myprintf("Success and no had_children. alpkt=%f, beta=%f.\n", alpkt, beta);
                result = SearchResult::from_eval(value, alpkt, beta);
		myprintf("Result validity is %i.\n"
			 "eval=%f, eval_with_bonus=%f\n"
			 "Move choices by policy: ",
			 result.valid(), result.eval_with_bonus(0.0f),
			 result.eval_with_bonus(node->get_eval_bonus()));
		print_move_choices_by_policy(currstate, *node, 3, 0.01f);
            }
        }
    }

    if (node->has_children() && !result.valid()) {
	myprintf("Result is not valid and node has children. "
		 "About to call uct_select_child().\n");
        auto next = node->uct_select_child(color, node == m_root.get());
	myprintf("About to call get_move().\n");
        auto move = next->get_move();
	myprintf("Move is %i. About to play move.\n", move);

        currstate.play_move(move);
        if (move != FastBoard::PASS && currstate.superko()) {
            next->invalidate();
        } else {
            std::string tmp = currstate.move_to_text(move);
	    //            myprintf("%4s ", tmp.c_str());
            myprintf("Move: %4s\n", tmp.c_str());
	    myprintf("About to call play_simulation().\n");
            result = play_simulation(currstate, next);
        }
    }

    //    extern bool is_mult_komi_net;
    if (result.valid()) {
	const auto eval = is_mult_komi_net ?
	    result.eval_with_bonus(node->get_eval_bonus()) : result.eval();
	myprintf("About to update blackevals with %f\n", eval);
        node->update(eval);
    }
    node->virtual_loss_undo();

    myprintf("Last move was %i, or %s. Simulation ends.\n"
	     "Visits=%i, blackevals=%f, eval=%f, net_eval=%f.\n",
	     lastmove, tmp.c_str(),
	     node->get_visits(),
	     node->get_blackevals(),
	     node->get_eval(color),
	     node->get_net_eval(color));

    
    return result;
}

void UCTSearch::dump_stats(FastState & state, UCTNode & parent) {
    if (cfg_quiet || !parent.has_children()) {
        return;
    }

    const int color = state.get_to_move();

    // sort children, put best move on top
    parent.sort_children(color);

    if (parent.get_first_child()->first_visit()) {
        return;
    }

    int movecount = 0;
    for (const auto& node : parent.get_children()) {
        // Always display at least two moves. In the case there is
        // only one move searched the user could get an idea why.
        if (++movecount > 2 && !node->get_visits()) break;

        std::string move = state.move_to_text(node->get_move());
        FastState tmpstate = state;
        tmpstate.play_move(node->get_move());
        std::string pv = move + " " + get_pv(tmpstate, *node);

        myprintf("%4s -> %7d (V: %5.2f%%) (N: %5.2f%%) PV: %s\n",
            move.c_str(),
            node->get_visits(),
            node->get_visits() ? node->get_eval(color)*100.0f : 0.0f,
            node->get_score() * 100.0f,
            pv.c_str());
    }
    tree_stats(parent);
}

void tree_stats_helper(const UCTNode& node, size_t depth,
                       size_t& nodes, size_t& non_leaf_nodes,
                       size_t& depth_sum, size_t& max_depth,
                       size_t& children_count) {
    nodes += 1;
    non_leaf_nodes += node.get_visits() > 1;
    depth_sum += depth;
    if (depth > max_depth) max_depth = depth;

    for (const auto& child : node.get_children()) {
        if (child.get_visits() > 0) {
            children_count += 1;
            tree_stats_helper(*(child.get()), depth+1,
                              nodes, non_leaf_nodes, depth_sum,
                              max_depth, children_count);
        } else {
            nodes += 1;
            depth_sum += depth+1;
            if (depth+1 > max_depth) max_depth = depth+1;
        }
    }
}

void UCTSearch::tree_stats(const UCTNode& node) {
    size_t nodes = 0;
    size_t non_leaf_nodes = 0;
    size_t depth_sum = 0;
    size_t max_depth = 0;
    size_t children_count = 0;
    tree_stats_helper(node, 0,
                      nodes, non_leaf_nodes, depth_sum,
                      max_depth, children_count);

    if (nodes > 0) {
        myprintf("%.1f average depth, %d max depth\n",
                 (1.0f*depth_sum) / nodes, max_depth);
        myprintf("%d non leaf nodes, %.2f average children\n",
                 non_leaf_nodes, (1.0f*children_count) / non_leaf_nodes);
    }
}

bool UCTSearch::should_resign(passflag_t passflag, float bestscore) {
    if (passflag & UCTSearch::NORESIGN) {
        // resign not allowed
        return false;
    }

    if (cfg_resignpct == 0) {
        // resign not allowed
        return false;
    }

    const size_t board_squares = m_rootstate.board.get_boardsize()
                               * m_rootstate.board.get_boardsize();
    const auto move_threshold = board_squares / 4;
    const auto movenum = m_rootstate.get_movenum();
    if (movenum <= move_threshold) {
        // too early in game to resign
        return false;
    }

    const auto color = m_rootstate.board.get_to_move();

    const auto is_default_cfg_resign = cfg_resignpct < 0;
    const auto resign_threshold =
        0.01f * (is_default_cfg_resign ? 10 : cfg_resignpct);
    if (bestscore > resign_threshold) {
        // eval > cfg_resign
        return false;
    }

    if ((m_rootstate.get_handicap() > 0)
            && (color == FastBoard::WHITE)
            && is_default_cfg_resign) {
        const auto handicap_resign_threshold =
            resign_threshold / (1 + m_rootstate.get_handicap());

        // Blend the thresholds for the first ~215 moves.
        auto blend_ratio = std::min(1.0f, movenum / (0.6f * board_squares));
        auto blended_resign_threshold = blend_ratio * resign_threshold
            + (1 - blend_ratio) * handicap_resign_threshold;
        if (bestscore > blended_resign_threshold) {
            // Allow lower eval for white in handicap games
            // where opp may fumble.
            return false;
        }
    }

    return true;
}

int UCTSearch::get_best_move(passflag_t passflag) {
    int color = m_rootstate.board.get_to_move();

    // Make sure best is first
    m_root->sort_children(color);

    // Check whether to randomize the best move proportional
    // to the playout counts, early game only.
    auto movenum = int(m_rootstate.get_movenum());
    myprintf("Check: this move is %s.\n", (m_rootstate.is_blunder() ? "blunder" : "ok") );	
    //m_rootstate.copy_last_rnd_move_num();
    if (movenum < cfg_random_cnt) {
	myprintf("About to call rnd_first...\n");
        const auto dumb_move_chosen = m_root->randomize_first_proportionally();
	myprintf("Done. Chosen move is %s.\n", (dumb_move_chosen ? "blunder" : "ok") );	
	if (should_resign(passflag, m_root->get_first_child()->get_eval(color))) {
	    myprintf("Random move would lead to immediate resignation... \n"
		     "Reverting to best move.\n");	    
	    m_root->sort_children(color);
	} else if (dumb_move_chosen) {
	    myprintf("Dumb move chosen.\n");
	    m_rootstate.set_blunder_state(true);
	}
    }
    myprintf("Check: last move is %s.\n", (m_rootstate.is_blunder() ? "blunder" : "ok") );	
    
    auto first_child = m_root->get_first_child();
    assert(first_child != nullptr);

    auto bestmove = first_child->get_move();
    auto bestscore = first_child->get_eval(color);

    // do we want to fiddle with the best move because of the rule set?
    if (passflag & UCTSearch::NOPASS) {
        // were we going to pass?
        if (bestmove == FastBoard::PASS) {
            UCTNode * nopass = m_root->get_nopass_child(m_rootstate);

            if (nopass != nullptr) {
                myprintf("Preferring not to pass.\n");
                bestmove = nopass->get_move();
                if (nopass->first_visit()) {
                    bestscore = 1.0f;
                } else {
                    bestscore = nopass->get_eval(color);
                }
            } else {
                myprintf("Pass is the only acceptable move.\n");
            }
        }
    } else {
        if (!cfg_dumbpass && bestmove == FastBoard::PASS) {
            // Either by forcing or coincidence passing is
            // on top...check whether passing loses instantly
            // do full count including dead stones.
            // In a reinforcement learning setup, it is possible for the
            // network to learn that, after passing in the tree, the two last
            // positions are identical, and this means the position is only won
            // if there are no dead stones in our own territory (because we use
            // Trump-Taylor scoring there). So strictly speaking, the next
            // heuristic isn't required for a pure RL network, and we have
            // a commandline option to disable the behavior during learning.
            // On the other hand, with a supervised learning setup, we fully
            // expect that the engine will pass out anything that looks like
            // a finished game even with dead stones on the board (because the
            // training games were using scoring with dead stone removal).
            // So in order to play games with a SL network, we need this
            // heuristic so the engine can "clean up" the board. It will still
            // only clean up the bare necessity to win. For full dead stone
            // removal, kgs-genmove_cleanup and the NOPASS mode must be used.
            float score = m_rootstate.final_score();
            // Do we lose by passing?
            if ((score > 0.0f && color == FastBoard::WHITE)
                ||
                (score < 0.0f && color == FastBoard::BLACK)) {
                myprintf("Passing loses :-(\n");
                // Find a valid non-pass move.
                UCTNode * nopass = m_root->get_nopass_child(m_rootstate);
                if (nopass != nullptr) {
                    myprintf("Avoiding pass because it loses.\n");
                    bestmove = nopass->get_move();
                    if (nopass->first_visit()) {
                        bestscore = 1.0f;
                    } else {
                        bestscore = nopass->get_eval(color);
                    }
                } else {
                    myprintf("No alternative to passing.\n");
                }
            } else {
                myprintf("Passing wins :-)\n");
            }
        } else if (!cfg_dumbpass
                   && m_rootstate.get_last_move() == FastBoard::PASS) {
            // Opponents last move was passing.
            // We didn't consider passing. Should we have and
            // end the game immediately?
            float score = m_rootstate.final_score();
            // do we lose by passing?
            if ((score > 0.0f && color == FastBoard::WHITE)
                ||
                (score < 0.0f && color == FastBoard::BLACK)) {
                myprintf("Passing loses, I'll play on.\n");
            } else {
                myprintf("Passing wins, I'll pass out.\n");
                bestmove = FastBoard::PASS;
            }
        }
    }

    // if we aren't passing, should we consider resigning?
    if (bestmove != FastBoard::PASS) {
        if (should_resign(passflag, bestscore)) {
            myprintf("Eval (%.2f%%) looks bad. Resigning.\n",
                     100.0f * bestscore);
            bestmove = FastBoard::RESIGN;
        }
    }

    return bestmove;
}

std::string UCTSearch::get_pv(FastState & state, UCTNode& parent) {
    if (!parent.has_children()) {
        return std::string();
    }

    auto& best_child = parent.get_best_root_child(state.get_to_move());
    if (best_child.first_visit()) {
        return std::string();
    }
    auto best_move = best_child.get_move();
    auto res = state.move_to_text(best_move);

    state.play_move(best_move);

    auto next = get_pv(state, best_child);
    if (!next.empty()) {
        res.append(" ").append(next);
    }
    return res;
}

void UCTSearch::dump_analysis(int playouts) {
    if (cfg_quiet) {
        return;
    }

    FastState tempstate = m_rootstate;
    int color = tempstate.board.get_to_move();

    std::string pvstring = get_pv(tempstate, *m_root);
    float winrate = 100.0f * m_root->get_eval(color);
    myprintf("Playouts: %d, Win: %5.2f%%, PV: %s\n",
             playouts, winrate, pvstring.c_str());
}

bool UCTSearch::is_running() const {
    return m_run && m_nodes < MAX_TREE_SIZE;
}

int UCTSearch::est_playouts_left(int elapsed_centis, int time_for_move) const {
    auto playouts = m_playouts.load();
    const auto playouts_left =
        std::max(0, std::min(m_maxplayouts - playouts,
                             m_maxvisits - m_root->get_visits()));

    // Wait for at least 1 second and 100 playouts
    // so we get a reliable playout_rate.
    if (elapsed_centis < 100 || playouts < 100) {
        return playouts_left;
    }
    const auto playout_rate = 1.0f * playouts / elapsed_centis;
    const auto time_left = std::max(0, time_for_move - elapsed_centis);
    return std::min(playouts_left,
                    static_cast<int>(std::ceil(playout_rate * time_left)));
}

size_t UCTSearch::prune_noncontenders(int elapsed_centis, int time_for_move) {
    auto Nfirst = 0;
    // There are no cases where the root's children vector gets modified
    // during a multithreaded search, so it is safe to walk it here without
    // taking the (root) node lock.
    for (const auto& node : m_root->get_children()) {
        if (node->valid()) {
            Nfirst = std::max(Nfirst, node->get_visits());
        }
    }
    const auto min_required_visits =
        Nfirst - est_playouts_left(elapsed_centis, time_for_move);
    auto pruned_nodes = size_t{0};
    for (const auto& node : m_root->get_children()) {
        if (node->valid()) {
            const auto has_enough_visits =
                node->get_visits() >= min_required_visits;

            node->set_active(has_enough_visits);
            if (!has_enough_visits) {
                ++pruned_nodes;
            }
        }
    }

    assert(pruned_nodes < m_root->get_children().size());
    return pruned_nodes;
}

bool UCTSearch::have_alternate_moves(int elapsed_centis, int time_for_move) {
    if (cfg_timemanage == TimeManagement::OFF) {
        return true;
    }
    auto pruned = prune_noncontenders(elapsed_centis, time_for_move);
    if (pruned < m_root->get_children().size() - 1) {
        return true;
    }
    // If we cannot save up time anyway, use all of it. This
    // behavior can be overruled by setting "fast" time management,
    // which will cause Leela to quickly respond to obvious/forced moves.
    // That comes at the cost of some playing strength as she now cannot
    // think ahead about her next moves in the remaining time.
    auto my_color = m_rootstate.get_to_move();
    auto tc = m_rootstate.get_timecontrol();
    if (!tc.can_accumulate_time(my_color)
        || m_maxplayouts < UCTSearch::UNLIMITED_PLAYOUTS) {
        if (cfg_timemanage != TimeManagement::FAST) {
            return true;
        }
    }
    // In a timed search we will essentially always exit because
    // the remaining time is too short to let another move win, so
    // avoid spamming this message every move. We'll print it if we
    // save at least half a second.
    if (time_for_move - elapsed_centis > 50) {
        myprintf("%.1fs left, stopping early.\n",
                    (time_for_move - elapsed_centis) / 100.0f);
    }
    return false;
}

bool UCTSearch::stop_thinking(int elapsed_centis, int time_for_move) const {
    return m_playouts >= m_maxplayouts
           || m_root->get_visits() >= m_maxvisits
           || elapsed_centis >= time_for_move;
}

void UCTWorker::operator()() {
    do {
        auto currstate = std::make_unique<GameState>(m_rootstate);
        auto result = m_search->play_simulation(*currstate, m_root);
        if (result.valid()) {
            m_search->increment_playouts();
        }
    } while (m_search->is_running());
}

void UCTSearch::increment_playouts() {
    m_playouts++;
    myprintf("\n");
}


void UCTSearch::print_move_choices_by_policy(KoState & state, UCTNode & parent, int at_least_as_many, float probab_threash) {
    parent.sort_children_by_policy();
    int movecount = 0;
    float policy_value_of_move = 1.0f;
    for (const auto& node : parent.get_children()) {
        if (++movecount > at_least_as_many && policy_value_of_move<probab_threash)
	    break;

	    policy_value_of_move = node.get_score();
        std::string tmp = state.move_to_text(node.get_move());
        myprintf("%4s %4.1f",
		 tmp.c_str(),
		 policy_value_of_move * 100.0f);
    }
    myprintf("\n");
}


int UCTSearch::think(int color, passflag_t passflag) {
    // Start counting time for us
    m_rootstate.start_clock(color);

    // set up timing info
    Time start;

    myprintf ("About to update root.\n");
    update_root();
    // set side to move
    m_rootstate.board.set_to_move(color);

    m_rootstate.get_timecontrol().set_boardsize(
        m_rootstate.board.get_boardsize());
    auto time_for_move = m_rootstate.get_timecontrol().max_time_for_move(color, m_rootstate.get_movenum());

    myprintf("Thinking at most %.1f seconds...\n", time_for_move/100.0f);

    // create a sorted list of legal moves (make sure we
    // play something legal and decent even in time trouble)
    int n=m_nodes;
    myprintf ("About to prepare root node. m_nodes=%i\n", n);
    m_root->prepare_root_node(color, m_nodes, m_rootstate);

    myprintf("We are at root. Move choices by policy are: ");
    print_move_choices_by_policy(m_rootstate, *m_root, 5, 0.01f);
    myprintf("\n");
    
    m_run = true;
    int cpus = cfg_num_threads;
    myprintf("cpus=%i\n", cpus);
    ThreadGroup tg(thread_pool);
    for (int i = 1; i < cpus; i++) {
      myprintf("About to add a UCTWorker...\n");
      tg.add_task(UCTWorker(m_rootstate, this, m_root.get()));
    }

    bool keeprunning = true;
    int last_update = 0;
    do {
        auto currstate = std::make_unique<GameState>(m_rootstate);

	myprintf("About to play simulation.\n");	
        auto result = play_simulation(*currstate, m_root.get());
	myprintf("Simulation ended.\n");	
        if (result.valid()) {
	  myprintf("Result is valid.\n");	
	  increment_playouts();
        }

        Time elapsed;
        int elapsed_centis = Time::timediff_centis(start, elapsed);

        // output some stats every few seconds
        // check if we should still search
        if (elapsed_centis - last_update > 250) {
            last_update = elapsed_centis;
            dump_analysis(static_cast<int>(m_playouts));
        }
        keeprunning  = is_running();
        keeprunning &= !stop_thinking(elapsed_centis, time_for_move);
        keeprunning &= have_alternate_moves(elapsed_centis, time_for_move);
    } while (keeprunning);

    // stop the search
    m_run = false;
    myprintf("About to wait all workers.\n");	
    tg.wait_all();

    // reactivate all pruned root children
    myprintf("About to reactivate pruned children. Counting ");	
    for (const auto& node : m_root->get_children()) {
      myprintf(".");	
        node->set_active(true);
    }
    myprintf(" finished.\n");

    m_rootstate.stop_clock(color);
    if (!m_root->has_children()) {
        return FastBoard::PASS;
    }

    // display search info
    myprintf("\n");
    dump_stats(m_rootstate, *m_root);

    myprintf("About to call get_best_move.\n");	
    int bestmove = get_best_move(passflag);

    myprintf("Writing training info.\n");
    Training::record(m_rootstate, *m_root);

    myprintf("Saving evaluation for black in current GameState:\n");
    const auto alpkt = m_root->get_net_alpkt();
    const auto beta = m_root->get_net_beta();
    m_rootstate.set_eval(alpkt, beta,
			 sigmoid(alpkt, beta, 0.0f),
			 m_root->get_eval(FastBoard::BLACK),
			 m_root->get_eval_bonus());

    const auto ev = m_rootstate.get_eval();
    myprintf("alpkt=%.2f, beta=%.3f, pi=%.3f, avg=%.3f, xbar=%.1f\n",
	     std::get<0>(ev),
	     std::get<1>(ev),
	     std::get<2>(ev),
	     std::get<3>(ev),
	     std::get<4>(ev));
    
    Time elapsed;
    int elapsed_centis = Time::timediff_centis(start, elapsed);
    if (elapsed_centis+1 > 0) {
        myprintf("%d visits, %d nodes, %d playouts, %.0f n/s\n\n",
                 m_root->get_visits(),
                 static_cast<int>(m_nodes),
                 static_cast<int>(m_playouts),
                 (m_playouts * 100.0) / (elapsed_centis+1));
    }

    // Copy the root state. Use to check for tree re-use in future calls.
    m_last_rootstate = std::make_unique<GameState>(m_rootstate);
    return bestmove;
}

void UCTSearch::ponder() {
    update_root();

    m_root->prepare_root_node(m_rootstate.board.get_to_move(),
                              m_nodes, m_rootstate);

    m_run = true;
    ThreadGroup tg(thread_pool);
    for (int i = 1; i < cfg_num_threads; i++) {
        tg.add_task(UCTWorker(m_rootstate, this, m_root.get()));
    }
    auto keeprunning = true;
    do {
        auto currstate = std::make_unique<GameState>(m_rootstate);
        auto result = play_simulation(*currstate, m_root.get());
        if (result.valid()) {
            increment_playouts();
        }
        keeprunning  = is_running();
        keeprunning &= !stop_thinking(0, 1);
    } while (!Utils::input_pending() && keeprunning);

    // stop the search
    m_run = false;
    tg.wait_all();

    // display search info
    myprintf("\n");
    dump_stats(m_rootstate, *m_root);

    myprintf("\n%d visits, %d nodes\n\n", m_root->get_visits(), m_nodes.load());

    // Copy the root state. Use to check for tree re-use in future calls.
    m_last_rootstate = std::make_unique<GameState>(m_rootstate);
}

void UCTSearch::set_playout_limit(int playouts) {
    static_assert(std::is_convertible<decltype(playouts),
                                      decltype(m_maxplayouts)>::value,
                  "Inconsistent types for playout amount.");
    m_maxplayouts = std::min(playouts, UNLIMITED_PLAYOUTS);
}

void UCTSearch::set_visit_limit(int visits) {
    static_assert(std::is_convertible<decltype(visits),
                                      decltype(m_maxvisits)>::value,
                  "Inconsistent types for visits amount.");
    // Limit to type max / 2 to prevent overflow when multithreading.
    m_maxvisits = std::min(visits, UNLIMITED_PLAYOUTS);
}

float SearchResult::eval_with_bonus(float xbar) {
    //    myprintf("Function eval_with_bonus: xbar=%f, alpkt=%f, beta=%f",
    //	     xbar, m_alpkt, m_beta);
    if (std::abs(xbar)<0.001f) {
	return sigmoid(m_alpkt,m_beta,0.0f);
    }
    else if ((std::abs(m_alpkt)+std::abs(xbar))*m_beta<10.0f) {
	return 1-std::log(sigmoid(m_alpkt,m_beta,xbar)/sigmoid(m_alpkt,m_beta,0.0f))/m_beta/xbar;
    }
    else if (m_alpkt>0.0f) {
	return 1;
    }
    else return 0;	
}
