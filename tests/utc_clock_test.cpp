#include "support/utc_clock.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

void require(bool condition, const char *message)
{
	if (!condition) {
		std::cerr << message << '\n';
		std::exit(1);
	}
}

void nanosecond_pll_state_uses_live_error_terms()
{
	timex state{};
	state.status = STA_PLL | STA_NANO;
	state.offset = -240'935;
	state.esterror = 7;
	state.precision = 1;
	state.maxerror = 113'000;

	const auto status =
		msap1::acquisition::daemon::utc_clock_status_from_timex(state);
	require(status.synchronized,
		"a disciplined nanosecond clock was reported unsynchronized");
	require(status.estimated_uncertainty_ns == 248'935,
		"nanosecond PLL uncertainty did not combine offset, estimate, and precision");
}

void maximum_error_growth_does_not_inflate_the_live_estimate()
{
	timex first{};
	first.status = STA_PLL | STA_NANO;
	first.offset = 125'000;
	first.precision = 1;
	first.maxerror = 1'000;
	auto later = first;
	later.maxerror = 500'000;

	const auto initial =
		msap1::acquisition::daemon::utc_clock_status_from_timex(first);
	const auto after_growth =
		msap1::acquisition::daemon::utc_clock_status_from_timex(later);
	require(initial.estimated_uncertainty_ns == 126'000 &&
			after_growth.estimated_uncertainty_ns ==
				initial.estimated_uncertainty_ns,
		"kernel maxerror growth leaked into the current uncertainty estimate");
}

void microsecond_mode_and_leap_state_are_preserved()
{
	timex state{};
	state.status = STA_PLL | STA_INS;
	state.offset = -240;
	state.esterror = 7;
	state.precision = 1;
	const auto positive =
		msap1::acquisition::daemon::utc_clock_status_from_timex(state);
	require(positive.synchronized && positive.positive_leap_pending &&
			!positive.negative_leap_pending &&
			positive.estimated_uncertainty_ns == 248'000,
		"microsecond-mode clock or positive leap state was decoded incorrectly");

	state.status = STA_UNSYNC | STA_DEL | STA_NANO;
	const auto negative =
		msap1::acquisition::daemon::utc_clock_status_from_timex(state);
	require(!negative.synchronized && !negative.positive_leap_pending &&
			negative.negative_leap_pending,
		"unsynchronized negative-leap state was promoted or lost");
}

void conversion_saturates_instead_of_wrapping()
{
	timex state{};
	state.status = STA_PLL;
	state.offset = std::numeric_limits<long>::max();
	state.esterror = std::numeric_limits<long>::max();
	state.precision = std::numeric_limits<long>::max();
	const auto status =
		msap1::acquisition::daemon::utc_clock_status_from_timex(state);
	require(status.estimated_uncertainty_ns ==
			std::numeric_limits<std::uint64_t>::max(),
		"overflowing clock error terms wrapped to a qualifying estimate");
}

} // namespace

int main()
{
	nanosecond_pll_state_uses_live_error_terms();
	maximum_error_growth_does_not_inflate_the_live_estimate();
	microsecond_mode_and_leap_state_are_preserved();
	conversion_saturates_instead_of_wrapping();
	return 0;
}
