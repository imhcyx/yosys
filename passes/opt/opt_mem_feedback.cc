/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Clifford Wolf <clifford@clifford.at>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "kernel/yosys.h"
#include "kernel/satgen.h"
#include "kernel/sigtools.h"
#include "kernel/modtools.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

bool memrd_cmp(RTLIL::Cell *a, RTLIL::Cell *b)
{
	return a->name < b->name;
}

bool memwr_cmp(RTLIL::Cell *a, RTLIL::Cell *b)
{
	return a->parameters.at(ID::PRIORITY).as_int() < b->parameters.at(ID::PRIORITY).as_int();
}

struct OptMemFeedbackWorker
{
	RTLIL::Design *design;
	RTLIL::Module *module;
	SigMap sigmap, sigmap_xmux;

	std::map<RTLIL::SigBit, std::pair<RTLIL::Cell*, int>> sig_to_mux;
	std::map<pair<std::set<std::map<SigBit, bool>>, SigBit>, SigBit> conditions_logic_cache;


	// -----------------------------------------------------------------
	// Converting feedbacks to async read ports to proper enable signals
	// -----------------------------------------------------------------

	bool find_data_feedback(const std::set<RTLIL::SigBit> &async_rd_bits, RTLIL::SigBit sig,
			std::map<RTLIL::SigBit, bool> &state, std::set<std::map<RTLIL::SigBit, bool>> &conditions)
	{
		if (async_rd_bits.count(sig)) {
			conditions.insert(state);
			return true;
		}

		if (sig_to_mux.count(sig) == 0)
			return false;

		RTLIL::Cell *cell = sig_to_mux.at(sig).first;
		int bit_idx = sig_to_mux.at(sig).second;

		std::vector<RTLIL::SigBit> sig_a = sigmap(cell->getPort(ID::A));
		std::vector<RTLIL::SigBit> sig_b = sigmap(cell->getPort(ID::B));
		std::vector<RTLIL::SigBit> sig_s = sigmap(cell->getPort(ID::S));
		std::vector<RTLIL::SigBit> sig_y = sigmap(cell->getPort(ID::Y));
		log_assert(sig_y.at(bit_idx) == sig);

		for (int i = 0; i < int(sig_s.size()); i++)
			if (state.count(sig_s[i]) && state.at(sig_s[i]) == true) {
				if (find_data_feedback(async_rd_bits, sig_b.at(bit_idx + i*sig_y.size()), state, conditions)) {
					RTLIL::SigSpec new_b = cell->getPort(ID::B);
					new_b.replace(bit_idx + i*sig_y.size(), RTLIL::State::Sx);
					cell->setPort(ID::B, new_b);
				}
				return false;
			}


		for (int i = 0; i < int(sig_s.size()); i++)
		{
			if (state.count(sig_s[i]) && state.at(sig_s[i]) == false)
				continue;

			std::map<RTLIL::SigBit, bool> new_state = state;
			new_state[sig_s[i]] = true;

			if (find_data_feedback(async_rd_bits, sig_b.at(bit_idx + i*sig_y.size()), new_state, conditions)) {
				RTLIL::SigSpec new_b = cell->getPort(ID::B);
				new_b.replace(bit_idx + i*sig_y.size(), RTLIL::State::Sx);
				cell->setPort(ID::B, new_b);
			}
		}

		std::map<RTLIL::SigBit, bool> new_state = state;
		for (int i = 0; i < int(sig_s.size()); i++)
			new_state[sig_s[i]] = false;

		if (find_data_feedback(async_rd_bits, sig_a.at(bit_idx), new_state, conditions)) {
			RTLIL::SigSpec new_a = cell->getPort(ID::A);
			new_a.replace(bit_idx, RTLIL::State::Sx);
			cell->setPort(ID::A, new_a);
		}

		return false;
	}

	RTLIL::SigBit conditions_to_logic(std::set<std::map<RTLIL::SigBit, bool>> &conditions, SigBit olden, int &created_conditions)
	{
		auto key = make_pair(conditions, olden);

		if (conditions_logic_cache.count(key))
			return conditions_logic_cache.at(key);

		RTLIL::SigSpec terms;
		for (auto &cond : conditions) {
			RTLIL::SigSpec sig1, sig2;
			for (auto &it : cond) {
				sig1.append(it.first);
				sig2.append(it.second ? RTLIL::State::S1 : RTLIL::State::S0);
			}
			terms.append(module->Ne(NEW_ID, sig1, sig2));
			created_conditions++;
		}

		if (olden.wire != nullptr || olden != State::S1)
			terms.append(olden);

		if (GetSize(terms) == 0)
			terms = State::S1;

		if (GetSize(terms) > 1)
			terms = module->ReduceAnd(NEW_ID, terms);

		return conditions_logic_cache[key] = terms;
	}

	void translate_rd_feedback_to_en(std::string memid, std::vector<RTLIL::Cell*> &rd_ports, std::vector<RTLIL::Cell*> &wr_ports)
	{
		std::map<RTLIL::SigSpec, std::vector<std::set<RTLIL::SigBit>>> async_rd_bits;
		std::map<RTLIL::SigBit, std::set<RTLIL::SigBit>> muxtree_upstream_map;
		std::set<RTLIL::SigBit> non_feedback_nets;

		for (auto wire : module->wires())
			if (wire->port_output) {
				std::vector<RTLIL::SigBit> bits = sigmap(wire);
				non_feedback_nets.insert(bits.begin(), bits.end());
			}

		for (auto cell : module->cells())
		{
			bool ignore_data_port = false;

			if (cell->type.in(ID($mux), ID($pmux)))
			{
				std::vector<RTLIL::SigBit> sig_a = sigmap(cell->getPort(ID::A));
				std::vector<RTLIL::SigBit> sig_b = sigmap(cell->getPort(ID::B));
				std::vector<RTLIL::SigBit> sig_s = sigmap(cell->getPort(ID::S));
				std::vector<RTLIL::SigBit> sig_y = sigmap(cell->getPort(ID::Y));

				non_feedback_nets.insert(sig_s.begin(), sig_s.end());

				for (int i = 0; i < int(sig_y.size()); i++) {
					muxtree_upstream_map[sig_y[i]].insert(sig_a[i]);
					for (int j = 0; j < int(sig_s.size()); j++)
						muxtree_upstream_map[sig_y[i]].insert(sig_b[i + j*sig_y.size()]);
				}

				continue;
			}

			if (cell->type.in(ID($memwr), ID($memrd)) &&
					cell->parameters.at(ID::MEMID).decode_string() == memid)
				ignore_data_port = true;

			for (auto conn : cell->connections())
			{
				if (ignore_data_port && conn.first == ID::DATA)
					continue;
				std::vector<RTLIL::SigBit> bits = sigmap(conn.second);
				non_feedback_nets.insert(bits.begin(), bits.end());
			}
		}

		std::set<RTLIL::SigBit> expand_non_feedback_nets = non_feedback_nets;
		while (!expand_non_feedback_nets.empty())
		{
			std::set<RTLIL::SigBit> new_expand_non_feedback_nets;

			for (auto &bit : expand_non_feedback_nets)
				if (muxtree_upstream_map.count(bit))
					for (auto &new_bit : muxtree_upstream_map.at(bit))
						if (!non_feedback_nets.count(new_bit)) {
							non_feedback_nets.insert(new_bit);
							new_expand_non_feedback_nets.insert(new_bit);
						}

			expand_non_feedback_nets.swap(new_expand_non_feedback_nets);
		}

		for (auto cell : rd_ports)
		{
			if (cell->parameters.at(ID::CLK_ENABLE).as_bool())
				continue;

			RTLIL::SigSpec sig_addr = sigmap(cell->getPort(ID::ADDR));
			std::vector<RTLIL::SigBit> sig_data = sigmap(cell->getPort(ID::DATA));

			for (int i = 0; i < int(sig_data.size()); i++)
				if (non_feedback_nets.count(sig_data[i]))
					goto not_pure_feedback_port;

			async_rd_bits[sig_addr].resize(max(async_rd_bits.size(), sig_data.size()));
			for (int i = 0; i < int(sig_data.size()); i++)
				async_rd_bits[sig_addr][i].insert(sig_data[i]);

		not_pure_feedback_port:;
		}

		if (async_rd_bits.empty())
			return;

		log("Populating enable bits on write ports of memory %s.%s with aync read feedback:\n", log_id(module), log_id(memid));

		for (auto cell : wr_ports)
		{
			RTLIL::SigSpec sig_addr = sigmap_xmux(cell->getPort(ID::ADDR));
			if (!async_rd_bits.count(sig_addr))
				continue;

			log("  Analyzing write port %s.\n", log_id(cell));

			std::vector<RTLIL::SigBit> cell_data = cell->getPort(ID::DATA);
			std::vector<RTLIL::SigBit> cell_en = cell->getPort(ID::EN);

			int created_conditions = 0;
			for (int i = 0; i < int(cell_data.size()); i++)
				if (cell_en[i] != RTLIL::SigBit(RTLIL::State::S0))
				{
					std::map<RTLIL::SigBit, bool> state;
					std::set<std::map<RTLIL::SigBit, bool>> conditions;

					find_data_feedback(async_rd_bits.at(sig_addr).at(i), cell_data[i], state, conditions);
					cell_en[i] = conditions_to_logic(conditions, cell_en[i], created_conditions);
				}

			if (created_conditions) {
				log("    Added enable logic for %d different cases.\n", created_conditions);
				cell->setPort(ID::EN, cell_en);
			}
		}
	}

	// -------------
	// Setup and run
	// -------------

	OptMemFeedbackWorker(RTLIL::Design *design) : design(design) {}

	void operator()(RTLIL::Module* module)
	{
		std::map<std::string, std::pair<std::vector<RTLIL::Cell*>, std::vector<RTLIL::Cell*>>> memindex;

		this->module = module;
		sigmap.set(module);
		sig_to_mux.clear();
		conditions_logic_cache.clear();

		sigmap_xmux = sigmap;
		for (auto cell : module->cells())
		{
			if (cell->type == ID($memrd))
				memindex[cell->parameters.at(ID::MEMID).decode_string()].first.push_back(cell);

			if (cell->type == ID($memwr))
				memindex[cell->parameters.at(ID::MEMID).decode_string()].second.push_back(cell);

			if (cell->type == ID($mux))
			{
				RTLIL::SigSpec sig_a = sigmap_xmux(cell->getPort(ID::A));
				RTLIL::SigSpec sig_b = sigmap_xmux(cell->getPort(ID::B));

				if (sig_a.is_fully_undef())
					sigmap_xmux.add(cell->getPort(ID::Y), sig_b);
				else if (sig_b.is_fully_undef())
					sigmap_xmux.add(cell->getPort(ID::Y), sig_a);
			}

			if (cell->type.in(ID($mux), ID($pmux)))
			{
				std::vector<RTLIL::SigBit> sig_y = sigmap(cell->getPort(ID::Y));
				for (int i = 0; i < int(sig_y.size()); i++)
					sig_to_mux[sig_y[i]] = std::pair<RTLIL::Cell*, int>(cell, i);
			}
		}

		for (auto &it : memindex) {
			std::sort(it.second.first.begin(), it.second.first.end(), memrd_cmp);
			std::sort(it.second.second.begin(), it.second.second.end(), memwr_cmp);
			translate_rd_feedback_to_en(it.first, it.second.first, it.second.second);
		}
	}
};

struct OptMemFeedbackPass : public Pass {
	OptMemFeedbackPass() : Pass("opt_mem_feedback", "convert memory read-to-write port feedback paths to write enables") { }
	void help() override
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    opt_mem_feedback [selection]\n");
		log("\n");
		log("This pass detects cases where an asynchronous read port is connected via\n");
		log("a mux tree to a write port with the same address.  When such a path is\n");
		log("found, it is replaced with a new condition on an enable signal, possibly\n");
		log("allowing for removal of the read port.\n");
		log("\n");
	}
	void execute(std::vector<std::string> args, RTLIL::Design *design) override {
		log_header(design, "Executing OPT_MEM_FEEDBACK pass (finding memory read-to-write feedback paths).\n");
		extra_args(args, 1, design);
		OptMemFeedbackWorker worker(design);

		for (auto module : design->selected_modules())
			worker(module);
	}
} OptMemFeedbackPass;

PRIVATE_NAMESPACE_END
