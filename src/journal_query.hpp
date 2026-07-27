#pragma once

#include "mnc/logging/journal_reader.hpp"

#include <span>
#include <vector>

namespace mnc::logging::detail {

bool matches_query(const Entry &entry, const Query &query) noexcept;

/*
 * Deterministic ordered-entry query used by unit tests to exercise the same
 * filtering, limit, and cursor-continuation contract as JournalReader.
 */
std::vector<Entry> bounded_page(std::span<const Entry> entries,
				const Query &query);

} // namespace mnc::logging::detail
