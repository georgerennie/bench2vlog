#include <iostream>
#include <string>
#include "aiger++.hpp"
#include "clipp/clipp.h"
#include "fmt/core.h"

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

	const auto net = [](const Aiger::Lit lit) -> std::string {
		if (aiger_is_constant(lit))
			return lit == aiger_false ? "1'b0" : "1'b1";
		return fmt::format("{}n{}", aiger_sign(lit) ? "~" : "", aiger_lit2var(lit));
	};

	fmt::println(R"(module top()");

	for (const auto input : Aiger::inputs(aig))
		fmt::println("\tinput wire {},", net(input.lit));
	fmt::println("\tinput wire clk");
	fmt::println(");");

	// Declare all nets
	for (const auto gate : Aiger::gates(aig))
		fmt::println("\twire {};", net(gate.lhs));
	for (const auto latch : Aiger::latches(aig))
		fmt::println("\treg {};", net(latch.lit));

	// Comb gates
	for (const auto gate : Aiger::gates(aig))
		fmt::println("assign {} = {} & {};", net(gate.lhs), net(gate.rhs0), net(gate.rhs1));

	fmt::println("always @(posedge clk) begin");
	for (const auto latch : Aiger::latches(aig))
		fmt::println("\t{} <= {};", net(latch.lit), net(latch.next));
	fmt::println("end");

	fmt::println("initial begin");
	for (const auto latch : Aiger::latches(aig))
		if (latch.reset != latch.lit)
			fmt::println("\t{} = {};", net(latch.lit), net(latch.reset));
	fmt::println("end");

	fmt::println("always @* begin");
	for (const auto constraint : Aiger::constraints(aig))
		fmt::println("\tassume({});", net(constraint.lit));
	for (const auto bad: Aiger::bads(aig))
		fmt::println("\tassert({});", net(aiger_not(bad.lit)));
	assert(aig->num_justice == 0);
	assert(aig->num_fairness == 0);
	fmt::println("end");

	fmt::println("endmodule");
}
