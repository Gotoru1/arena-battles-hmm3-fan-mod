// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <limits>
#include <boost/multiprecision/cpp_int.hpp>

namespace evolution
{
inline int64_t damageProduct(int64_t damage, int64_t multiplier, int divisor = 1)
{
	if(damage <= 0 || multiplier <= 0) return 0;
	using Wide = boost::multiprecision::int128_t;
	const Wide value = Wide(damage) * multiplier / divisor;
	return value > std::numeric_limits<int64_t>::max() ? std::numeric_limits<int64_t>::max() : value.convert_to<int64_t>();
}
inline int64_t addDamage(int64_t a, int64_t b)
{
	if(b <= 0) return a;
	return a > std::numeric_limits<int64_t>::max()-b ? std::numeric_limits<int64_t>::max() : a+b;
}
}
