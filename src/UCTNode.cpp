/*
    This file is part of Leela Zero.
    Copyright (C) 2017-2018 Gian-Carlo Pascutto
    Copyright (C) 2018 SAI Team

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

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

#include "UCTNode.h"
#include "FastBoard.h"
#include "FastState.h"
#include "GTP.h"
#include "GameState.h"
#include "Network.h"
#include "Utils.h"

using namespace Utils;

UCTNode::UCTNode(int vertex, float score) : m_move(vertex), m_score(score) {
}

bool UCTNode::first_visit() const {
    return m_visits == 0;
}

SMP::Mutex& UCTNode::get_mutex() {
    return m_nodemutex;
}

bool UCTNode::create_children(std::atomic<int>& nodecount,
                              GameState& state,
			      float& value,
                              float& alpkt,
			      float& beta,
                              float min_psa_ratio) {
    // check whether somebody beat us to it (atomic)
    if (!expandable(min_psa_ratio)) {
        return false;
    }
    // acquire the lock
    LOCK(get_mutex(), lock);
    // no successors in final state
    if (state.get_passes() >= 2) {
        return false;
    }
    // check whether somebody beat us to it (after taking the lock)
    if (!expandable(min_psa_ratio)) {
        return false;
    }
    // Someone else is running the expansion
    if (m_is_expanding) {
        return false;
    }
    // We'll be the one queueing this node for expansion, stop others
    m_is_expanding = true;
    lock.unlock();

    const auto raw_netlist = Network::get_scored_moves(
        &state, Network::Ensemble::RANDOM_SYMMETRY);

    const auto to_move = state.board.get_to_move();
    const auto komi = state.get_komi();

    //    alpkt = m_net_alpkt = raw_netlist.alpha +
    //	(state.board.black_to_move() ? -komi : komi);
    alpkt = m_net_alpkt = (state.board.black_to_move() ? raw_netlist.alpha : -raw_netlist.alpha) - komi;
    beta = m_net_beta = raw_netlist.beta;
    value = raw_netlist.value; // = m_net_value

    // DCNN returns value as side to move
    // our search functions evaluate from black's point of view
    if (state.board.white_to_move())
        value = 1.0f - value;

    if (is_mult_komi_net) {
        const auto pi = sigmoid(alpkt, beta, 0.0f);
	// if pi is near to 1, this is much more precise than 1-pi
	const auto one_m_pi = sigmoid(-alpkt, beta, 0.0f);

    const auto pi_lambda = (1-cfg_lambda)*pi + cfg_lambda*0.5f;
    const auto pi_mu = (1-cfg_mu)*pi + cfg_mu*0.5f;

	// this is useful when lambda is near to 0 and pi near 1
	const auto one_m_pi_lambda = (1-cfg_lambda)*one_m_pi + cfg_lambda*0.5f;
	const auto sigma_inv_pi_lambda = std::log(pi_lambda) - std::log(one_m_pi_lambda);
	m_eval_bonus = sigma_inv_pi_lambda / beta - alpkt;
    const auto one_m_pi_mu = (1-cfg_mu)*one_m_pi + cfg_mu*0.5f;
	const auto sigma_inv_pi_mu = std::log(pi_mu) - std::log(one_m_pi_mu);
	m_eval_base = sigma_inv_pi_mu / beta - alpkt;
	m_agent_eval = Utils::sigmoid_interval_avg(alpkt, beta, m_eval_base, m_eval_bonus);

#ifndef NDEBUG
        myprintf("alpha=%f, beta=%f, pass=%f\n"
            "alpkt=%f, pi=%f, pi_lambda=%f, pi_mu=%f, x_bar=%f\n x_base=%f\n",
            raw_netlist.alpha, raw_netlist.beta, raw_netlist.policy_pass,
            m_net_alpkt, pi, pi_lambda, pi_mu, m_eval_bonus, m_eval_base);
#endif

        m_net_eval = pi;
    }
    else {
        m_eval_bonus = 0.0f;
        m_eval_base = 0.0f;
        m_net_eval = value;
	m_agent_eval = value;
    }

    std::vector<Network::ScoreVertexPair> nodelist;

    auto legal_sum = 0.0f;
    for (auto i = 0; i < BOARD_SQUARES; i++) {
        const auto x = i % BOARD_SIZE;
        const auto y = i / BOARD_SIZE;
        const auto vertex = state.board.get_vertex(x, y);
        if (state.is_move_legal(to_move, vertex)) {
            nodelist.emplace_back(raw_netlist.policy[i], vertex);
            legal_sum += raw_netlist.policy[i];
        }
    }
    nodelist.emplace_back(raw_netlist.policy_pass, FastBoard::PASS);
    legal_sum += raw_netlist.policy_pass;

    if (legal_sum > std::numeric_limits<float>::min()) {
        // re-normalize after removing illegal moves.
        for (auto& node : nodelist) {
            node.first /= legal_sum;
        }
    } else {
        // This can happen with new randomized nets.
        auto uniform_prob = 1.0f / nodelist.size();
        for (auto& node : nodelist) {
            node.first = uniform_prob;
        }
    }

    link_nodelist(nodecount, nodelist, min_psa_ratio);
    return true;
}

void UCTNode::link_nodelist(std::atomic<int>& nodecount,
                            std::vector<Network::ScoreVertexPair>& nodelist,
                            float min_psa_ratio) {
    assert(min_psa_ratio < m_min_psa_ratio_children);

    if (nodelist.empty()) {
        return;
    }

    // Use best to worst order, so highest go first
    std::stable_sort(rbegin(nodelist), rend(nodelist));

    LOCK(get_mutex(), lock);

    const auto max_psa = nodelist[0].first;
    const auto old_min_psa = max_psa * m_min_psa_ratio_children;
    const auto new_min_psa = max_psa * min_psa_ratio;
    if (new_min_psa > 0.0f) {
        m_children.reserve(
            std::count_if(cbegin(nodelist), cend(nodelist),
                [=](const auto& node) { return node.first >= new_min_psa; }
            )
        );
    } else {
        m_children.reserve(nodelist.size());
    }

    auto skipped_children = false;
    for (const auto& node : nodelist) {
        if (node.first < new_min_psa) {
            skipped_children = true;
        } else if (node.first < old_min_psa) {
            m_children.emplace_back(node.second, node.first);
            ++nodecount;
        }
    }

    m_min_psa_ratio_children = skipped_children ? min_psa_ratio : 0.0f;
    m_is_expanding = false;
}

const std::vector<UCTNodePointer>& UCTNode::get_children() const {
    return m_children;
}


int UCTNode::get_move() const {
    return m_move;
}

void UCTNode::virtual_loss() {
    m_virtual_loss += VIRTUAL_LOSS_COUNT;
}

void UCTNode::virtual_loss_undo() {
    m_virtual_loss -= VIRTUAL_LOSS_COUNT;
}

void UCTNode::update(float eval) {
    m_visits++;
    accumulate_eval(eval);
}

bool UCTNode::has_children() const {
    return m_min_psa_ratio_children <= 1.0f;
}

bool UCTNode::expandable(const float min_psa_ratio) const {
    return min_psa_ratio < m_min_psa_ratio_children;
}

float UCTNode::get_score() const {
    return m_score;
}

float UCTNode::get_eval_bonus() const {
    return m_eval_bonus;
}

float UCTNode::get_eval_bonus_father() const {
    return m_eval_bonus_father;
}

void UCTNode::set_eval_bonus_father(float bonus) {
    m_eval_bonus_father = bonus;
}

float UCTNode::get_eval_base() const {
    return m_eval_base;
}

float UCTNode::get_eval_base_father() const {
    return m_eval_base_father;
}

void UCTNode::set_eval_base_father(float bonus) {
    m_eval_base_father = bonus;
}

float UCTNode::get_net_eval() const {
    return m_net_eval;
}

float UCTNode::get_net_beta() const {
    return m_net_beta;
}

float UCTNode::get_net_alpkt() const {
    return m_net_alpkt;
}

void UCTNode::set_score(float score) {
    m_score = score;
}

int UCTNode::get_visits() const {
    return m_visits;
}

float UCTNode::get_eval(int tomove) const {
    // Due to the use of atomic updates and virtual losses, it is
    // possible for the visit count to change underneath us. Make sure
    // to return a consistent result to the caller by caching the values.
    auto virtual_loss = int{m_virtual_loss};
    auto visits = get_visits() + virtual_loss;
    assert(visits > 0);
    auto blackeval = get_blackevals();
    if (tomove == FastBoard::WHITE) {
        blackeval += static_cast<double>(virtual_loss);
    }
    auto score = static_cast<float>(blackeval / double(visits));
    if (tomove == FastBoard::WHITE) {
        score = 1.0f - score;
    }
    return score;
}

float UCTNode::get_net_eval(int tomove) const {
    if (tomove == FastBoard::WHITE) {
        return 1.0f - m_net_eval;
    }
    return m_net_eval;
}

float UCTNode::get_agent_eval(int tomove) const {
    if (tomove == FastBoard::WHITE) {
        return 1.0f - m_agent_eval;
    }
    return m_agent_eval;
}

double UCTNode::get_blackevals() const {
    return m_blackevals;
}

void UCTNode::accumulate_eval(float eval) {
    atomic_add(m_blackevals, double(eval));
}

UCTNode* UCTNode::uct_select_child(int color, bool is_root) {
    LOCK(get_mutex(), lock);

    // Count parentvisits manually to avoid issues with transpositions.
    auto total_visited_policy = 0.0f;
    auto parentvisits = size_t{0};
    for (const auto& child : m_children) {
        if (child.valid()) {
            parentvisits += child.get_visits();
            if (child.get_visits() > 0) {
                total_visited_policy += child.get_score();
            }
        }
    }

    auto numerator = std::sqrt(double(parentvisits));
    auto fpu_reduction = 0.0f;
    // Lower the expected eval for moves that are likely not the best.
    // Do not do this if we have introduced noise at this node exactly
    // to explore more.
    if (!is_root || !cfg_noise) {
        fpu_reduction = cfg_fpu_reduction * std::sqrt(total_visited_policy);
    }
    // Estimated eval for unknown nodes = original parent NN eval - reduction
    auto fpu_eval = 0.5f;
    if ( !cfg_fpuzero ) {
	fpu_eval = get_agent_eval(color) - fpu_reduction;
    }

    auto best = static_cast<UCTNodePointer*>(nullptr);
    auto best_value = std::numeric_limits<double>::lowest();

    for (auto& child : m_children) {
        if (!child.active()) {
            continue;
        }

        auto winrate = fpu_eval;
        if (child.get_visits() > 0) {
            winrate = child.get_eval(color);
        }
        auto psa = child.get_score();
        auto denom = 1.0 + child.get_visits();
        auto puct = cfg_puct * psa * (numerator / denom);
        auto value = winrate + puct;
        assert(value > std::numeric_limits<double>::lowest());

        if (value > best_value) {
            best_value = value;
            best = &child;
	}
    }

    assert(best != nullptr);
    best->inflate();
    return best->get();
}

class NodeComp : public std::binary_function<UCTNodePointer&,
                                             UCTNodePointer&, bool> {
public:
    NodeComp(int color) : m_color(color) {};
    bool operator()(const UCTNodePointer& a,
                    const UCTNodePointer& b) {
        // if visits are not same, sort on visits
        if (a.get_visits() != b.get_visits()) {
            return a.get_visits() < b.get_visits();
        }

        // neither has visits, sort on prior score
        if (a.get_visits() == 0) {
            return a.get_score() < b.get_score();
        }

        // both have same non-zero number of visits
        return a.get_eval(m_color) < b.get_eval(m_color);
    }
private:
    int m_color;
};

void UCTNode::sort_children(int color) {
    LOCK(get_mutex(), lock);
    std::stable_sort(rbegin(m_children), rend(m_children), NodeComp(color));
}

class NodeCompByPolicy : public std::binary_function<UCTNodePointer&,
                                             UCTNodePointer&, bool> {
public:
    bool operator()(const UCTNodePointer& a,
                    const UCTNodePointer& b) {
        return a.get_score() < b.get_score();
    }
};

void UCTNode::sort_children_by_policy() {
    LOCK(get_mutex(), lock);
    std::stable_sort(rbegin(m_children), rend(m_children), NodeCompByPolicy());
}

UCTNode& UCTNode::get_best_root_child(int color) {
    LOCK(get_mutex(), lock);
    assert(!m_children.empty());

    auto ret = std::max_element(begin(m_children), end(m_children),
                                NodeComp(color));
    ret->inflate();
    return *(ret->get());
}

size_t UCTNode::count_nodes() const {
    auto nodecount = size_t{0};
    nodecount += m_children.size();
    for (auto& child : m_children) {
        if (child.get_visits() > 0) {
            nodecount += child->count_nodes();
        }
    }
    return nodecount;
}

void UCTNode::invalidate() {
    m_status = INVALID;
}

void UCTNode::set_active(const bool active) {
    if (valid()) {
        m_status = active ? ACTIVE : PRUNED;
    }
}

bool UCTNode::valid() const {
    return m_status != INVALID;
}

bool UCTNode::active() const {
    return m_status == ACTIVE;
}
