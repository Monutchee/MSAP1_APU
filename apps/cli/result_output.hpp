#pragma once

#include "cli.hpp"

#include <glaze/glaze.hpp>

#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace msap1::cli {

/*
 * Commands collect one typed result and hand it to one of these generators.
 * Keeping collection separate from presentation prevents the human and
 * machine interfaces from drifting into two independent implementations.
 */
template <typename Result>
class ResultGenerator {
public:
	virtual ~ResultGenerator() = default;
	virtual int write(const Result &result, std::ostream &output) const = 0;
};

template <typename Data>
struct JsonSuccessEnvelope {
	std::string schema = "mnc.response.v1";
	bool success = true;
	Data data;
};

template <typename Data>
void write_json_success(std::ostream &output, Data data)
{
	auto json = glz::write_json(
		JsonSuccessEnvelope<Data>{.data = std::move(data)});
	if (!json)
		throw std::runtime_error("failed to serialize JSON response");
	output << *json << '\n';
}

template <typename Result, typename TextGenerator, typename JsonGenerator>
int render_result(const Options &options, const Result &result,
		  std::ostream &output, const TextGenerator &text,
		  const JsonGenerator &json)
{
	const ResultGenerator<Result> &generator =
		options.output_format == OutputFormat::json
			? static_cast<const ResultGenerator<Result> &>(json)
			: static_cast<const ResultGenerator<Result> &>(text);
	return generator.write(result, output);
}

} // namespace msap1::cli
