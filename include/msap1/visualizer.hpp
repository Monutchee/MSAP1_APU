#ifndef MSAP1_APU_VISUALIZER_HPP
#define MSAP1_APU_VISUALIZER_HPP

#include "msap1/protocol.hpp"

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace msap1 {

enum class OutputFormat {
	terminal,
	table,
	csv,
	json_lines,
};

OutputFormat parse_output_format(const std::string &name);

class Visualizer {
public:
	Visualizer(std::ostream &output, OutputFormat format,
		   std::vector<std::size_t> channels = {});
	void render(const AdcBatch &batch);
	void finish();

private:
	void render_terminal(const AdcBatch &batch);
	void render_rows(const AdcBatch &batch);

	std::ostream &output_;
	OutputFormat format_;
	std::vector<std::size_t> channels_;
	bool header_written_ = false;
	bool terminal_rendered_ = false;
	std::uint64_t terminal_next_frame_ = 0;
	std::uint64_t batches_seen_ = 0;
	std::uint64_t frames_seen_ = 0;
};

} // namespace msap1

#endif
