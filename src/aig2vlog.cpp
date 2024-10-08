#include <cassert>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ranges>
#include <string>
#include "aiger++.hpp"
#include "clipp/clipp.h"
#include "fmt/core.h"
#include "inja/inja.hpp"

template <class... Ts>
struct overloaded : Ts... {
	using Ts::operator()...;
};

int main(int argc, char* argv[]) {
	using namespace clipp;

	struct Options {
		bool show_help = false;
		std::string input_path;
		std::string output_path;
		std::string top_name;
		std::string internal_prefix = "_";
		bool ignore_symbols = false;
	} opt;

	// clang-format off
	auto cli = (
		option("-h", "--help")                % "show this message and exit" >> opt.show_help,
		value("input file",  opt.input_path)  % "input .aag or .aig file to translate",
		value("output file", opt.output_path) % "output .aag or .aig file to translate",
		(option("-t", "--top") & value("name", opt.top_name)) % "name for top module, filename by default",
		(option("-p", "--prefix") & value("prefix", opt.internal_prefix)) % "prefix for unnamed nets",
		option("-i", "--ignore-symbols")      % "don't use symbol names in the generated verilog" >> opt.ignore_symbols
	);
	// clang-format on

	opt.show_help |= !parse(argc, argv, cli);

	if (opt.show_help) {
		const auto format = doc_formatting{}.doc_column(28);
		std::cout << make_man_page(cli, "aig2vlog", format);
		return EXIT_FAILURE;
	}

	const auto aig = Aiger::checked_read(opt.input_path);
	if (!aig)
		return EXIT_FAILURE;

	if (aig->num_justice > 0 || aig->num_fairness > 0) {
		fmt::println(stderr, "ERROR: Justice and Fairness conditions not currently supported");
		return EXIT_FAILURE;
	}

	const auto path_top =
	    opt.input_path == "-" ? "top" : std::filesystem::path{opt.input_path}.stem().string();
	const auto module_name = !opt.top_name.empty() ? opt.top_name : path_top;

	const auto esc = [](const std::string s) {
		const auto simple = std::all_of(s.begin(), s.end(), [](const char c) {
			return isalnum(c) || c == '$' || c == '_';
		});
		return simple ? s : fmt::format(R"(\{} )", s);
	};

	const auto priv = [&](const auto s, const char* prefix = nullptr) {
		return fmt::format("{}{}{}", opt.internal_prefix, prefix ? prefix : "", s);
	};

	const auto name_symbs = [&](const auto span, const char* prefix) {
		for (auto it = span.begin(); it != span.end(); it++) {
			if (it->name) {
				if (!opt.ignore_symbols)
					continue;
				free(it->name);
			}

			// Create name symb and allocate memory for it with malloc
			const auto name = priv(std::distance(span.begin(), it), prefix);
			char* dst = reinterpret_cast<char*>(malloc(name.size() + 1));
			assert(dst != NULL);
			strcpy(dst, name.c_str());
			it->name = dst;
		}
	};

	// Name all symbols corresponding to their type
	name_symbs(Aiger::inputs(aig), "i");
	name_symbs(Aiger::outputs(aig), "o");
	name_symbs(Aiger::latches(aig), "l");
	name_symbs(Aiger::constraints(aig), "c");
	name_symbs(Aiger::bads(aig), "b");
	name_symbs(Aiger::justices(aig), "j");
	name_symbs(Aiger::fairnesses(aig), "f");

	const auto lhs = [&](const Aiger::Lit lit) -> std::string {
		assert(!aiger_is_constant(lit));
		assert(!aiger_sign(lit));
		if (auto* input = aiger_is_input(aig.get(), lit))
			return esc(input->name);
		if (auto* latch = aiger_is_latch(aig.get(), lit))
			return esc(latch->name);
		return esc(priv(lit, "n"));
	};

	const auto rhs = [&](const Aiger::Lit lit) -> std::string {
		if (aiger_is_constant(lit))
			return lit == aiger_true ? "1'b1" : "1'b0";
		return fmt::format("{}{}", aiger_sign(lit) ? "~" : "", lhs(aiger_strip(lit)));
	};

	const auto to_json = overloaded{
	    [&](const aiger_symbol& symb) -> inja::json {
		    assert(symb.name);
		    inja::json json{
		        {"lit", rhs(symb.lit)},
		        {"next", rhs(symb.next)},
		        {"prop_prefix", fmt::format("{}: ", symb.name)},
		        {"name", esc(symb.name)}
		    };

		    if (symb.reset != symb.next)
			    json["reset"] = rhs(symb.reset);

		    return json;
	    },
	    [&](const aiger_and& gate) -> inja::json {
		    return {
		        {"lhs", lhs(gate.lhs)},
		        {"rhs0", rhs(gate.rhs0)},
		        {"rhs1", rhs(gate.rhs1)},
		    };
	    }
	};

	const auto transform = [&](const auto symb_it) -> std::vector<inja::json> {
		const auto transformed = symb_it | std::views::transform(to_json);
		return {transformed.begin(), transformed.end()};
	};

	std::ofstream output_file;
	std::ostream* output_stream = &std::cout;
	if (opt.output_path != "-") {
		output_file.open(opt.output_path);
		output_stream = &output_file;
	}

	inja::render_to(
	    *output_stream, R"""(
// AUTOGENERATED WITH aig2vlog!!!

module {{ module_name }}(
## for input in inputs
	input wire {{ input.name }},
## endfor
## for output in outputs
	output wire {{ output.name }},
## endfor
	input wire {{ clk }},
	input wire {{ rst }}
);

// Latch declarations
## for latch in latches
reg {{ latch.lit }};
## endfor

// AND gates
{# declare gate wires before defining in case they are not topo sorted #}
## for gate in gates
wire {{ gate.lhs }};
## endfor
## for gate in gates
assign {{ gate.lhs }} = {{ gate.rhs0 }} & {{ gate.rhs1 }};
## endfor

// Latch definitions
always @(posedge {{ clk }}) begin
## for latch in latches
	{{ latch.lit }} <= {{ latch.next }};
## endfor
	if ({{ rst }}) begin
## for latch in latches
{% if existsIn(latch, "reset") %}		{{ latch.lit }} <= {{ latch.reset }};
{% endif -%}
## endfor
	end
end

// Assign outputs
## for output in outputs
	assign {{ output.name }} = {{ output.lit }};
## endfor

always @* begin
	if (~{{ rst }}) begin
		// Constraints
## for constraint in constraints
		{{ constraint.prop_prefix }}assume({{ constraint.lit }});
## endfor

		// Safety properties (bad)
## for assert in asserts
		{{ assert.prop_prefix }}assert(~{{ assert.lit }});
## endfor
	end
end

`ifdef YOSYS
`ifdef FORMAL
`define INITIAL_ASSUME_RESET
`endif
`endif

`ifdef INITIAL_ASSUME_RESET
initial assume({{ rst }});
`endif
endmodule
)""",
	    {{"module_name", esc(module_name)},
	     {"clk", esc("aig2vlog_clk")},
	     {"rst", esc("aig2vlog_rst")},
	     {"inputs", transform(Aiger::inputs(aig))},
	     {"outputs", transform(Aiger::outputs(aig))},
	     {"latches", transform(Aiger::latches(aig))},
	     {"gates", transform(Aiger::gates(aig))},
	     {"constraints", transform(Aiger::constraints(aig))},
	     {"asserts", transform(Aiger::bads(aig))}}
	);
}
