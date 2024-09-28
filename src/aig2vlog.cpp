#include <iostream>
#include <string>
#include "aiger++.hpp"
#include "clipp/clipp.h"
#include "fmt/core.h"
#include <cassert>

struct Options {
	std::string input_path;
	std::string output_path;
};

int main(int argc, char* argv[]) {
	using namespace clipp;

	Options opt;
	bool show_help = false;

	// clang-format off
	auto cli = (
		option("-h", "--help")                % "show this message and exit" >> show_help,
		value("input file",  opt.input_path)  % "input .aag or .aig file to translate",
		value("output file", opt.output_path) % "output .aag or .aig file to translate"
	);
	// clang-format on

	show_help |= !parse(argc, argv, cli);

	if (show_help) {
		const auto format = doc_formatting{}.doc_column(28);
		std::cout << make_man_page(cli, "aig2vlog", format);
		return EXIT_FAILURE;
	}

	const auto aig = Aiger::checked_read(opt.input_path);
	if (!aig)
		return EXIT_FAILURE;

	const auto lhs = [](const Aiger::Lit lit) -> std::string {
		assert(!aiger_is_constant(lit));
		assert(!aiger_sign(lit));
		return fmt::format("_n{}", aiger_lit2var(lit));
	};

	const auto rhs = [&lhs](const Aiger::Lit lit) -> std::string {
		if (aiger_is_constant(lit))
			return lit == aiger_false ? "1'b0" : "1'b1";
		return fmt::format("{}{}", aiger_sign(lit) ? "~" : "", lhs(aiger_strip(lit)));
	};

	fmt::println(R"(module top()");

	const auto net_name = [](const size_t idx, const char* name, const char prefix) -> std::string {
		if (name == nullptr)
			return fmt::format("_{}{}", prefix, idx);
		return fmt::format(R"(\{} )", name);
	};

	for (size_t i = 0; i < aig->num_inputs; i++)
		fmt::println("\tinput wire {},", net_name(i, aig->inputs[i].name, 'i'));
	for (size_t i = 0; i < aig->num_outputs; i++)
		fmt::println("\toutput wire {},", net_name(i, aig->outputs[i].name, 'o'));
	fmt::println("\tinput wire clk,");
	fmt::println("\tinput wire rst");
	fmt::println(");");

	// Connect inputs to internal wires
	for (size_t i = 0; i < aig->num_inputs; i++)
		fmt::println("wire {} = {};", lhs(aig->inputs[i].lit),net_name(i, aig->inputs[i].name, 'i'));

	// Declare all registers
	for (size_t i = 0; i < aig->num_latches; i++) {
		const auto& latch = aig->latches[i];
		const auto name = net_name(i, latch.name, 'l');
		fmt::println("reg {}; wire {} = {};", name, lhs(latch.lit), name);
	}

	// Comb gates
	for (const auto& gate : Aiger::gates(aig))
		fmt::println("wire {} = {} & {};", lhs(gate.lhs), rhs(gate.rhs0), rhs(gate.rhs1));

	fmt::println("always @(posedge clk) begin");
	for (size_t i = 0; i < aig->num_latches; i++)
		fmt::println("\t{} <= {};", net_name(i, aig->latches[i].name, 'l'), rhs(aig->latches[i].next));
	fmt::println("\tif (rst) begin");
	for (size_t i = 0; i < aig->num_latches; i++) {
		const auto& latch = aig->latches[i];
		if (latch.reset != latch.lit)
			fmt::println("\t\t{} <= {};", net_name(i, aig->latches[i].name, 'l'), rhs(latch.reset));
	}
	fmt::println("\tend");
	fmt::println("end");

	const auto prop_name = [](const char* name) -> std::string {
		if (name == nullptr)
			return "";
		else
			return fmt::format("{}: ", name);
	};

	// Connect internal wires to outputs
	for (size_t i = 0; i < aig->num_outputs; i++)
		fmt::println("assign {} = {};", net_name(i, aig->outputs[i].name, 'i'), rhs(aig->outputs[i].lit));

	fmt::println("always @* begin");
	fmt::println("\tif (!rst) begin");
	for (const auto& constraint : Aiger::constraints(aig))
		fmt::println("\t\t{}assume({});", prop_name(constraint.name), rhs(constraint.lit));
	for (const auto& bad: Aiger::bads(aig))
		fmt::println("\t\t{}assert({});", prop_name(bad.name), rhs(aiger_not(bad.lit)));
	assert(aig->num_justice == 0);
	assert(aig->num_fairness == 0);
	fmt::println("\tend");
	fmt::println("end");

	fmt::println("endmodule");
}
