/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/block_range_information.h"

#include <algorithm>

#include <boost/icl/interval_set.hpp>
#include <mcl/stdint.hpp>
#include <tsl/robin_set.h>

namespace Dynarmic::Backend {

template<typename ProgramCounterType>
void BlockRangeInformation<ProgramCounterType>::AddRange(boost::icl::discrete_interval<ProgramCounterType> range, IR::LocationDescriptor location) {
    if (boost::icl::is_empty(range)) {
        return;
    }
    const ProgramCounterType first = boost::icl::first(range);
    const ProgramCounterType last = boost::icl::last(range);
    const auto [begin, end] = block_ranges.equal_range(first);
    for (auto it = begin; it != end; ++it) {
        if (it->second == std::make_pair(last, location)) {
            return;
        }
    }
    block_ranges.emplace(first, std::make_pair(last, location));
    max_span = std::max<ProgramCounterType>(max_span, last - first);
}

template<typename ProgramCounterType>
void BlockRangeInformation<ProgramCounterType>::ClearCache() {
    block_ranges.clear();
    max_span = 0;
}

template<typename ProgramCounterType>
tsl::robin_set<IR::LocationDescriptor> BlockRangeInformation<ProgramCounterType>::InvalidateRanges(const boost::icl::interval_set<ProgramCounterType>& ranges) {
    tsl::robin_set<IR::LocationDescriptor> erase_locations;
    for (const auto& invalidate_interval : ranges) {
        if (boost::icl::is_empty(invalidate_interval)) {
            continue;
        }
        const ProgramCounterType first = boost::icl::first(invalidate_interval);
        const ProgramCounterType last = boost::icl::last(invalidate_interval);
        const ProgramCounterType lowest_start = first > max_span ? first - max_span : 0;
        for (auto it = block_ranges.lower_bound(lowest_start); it != block_ranges.end() && it->first <= last;) {
            if (it->second.first >= first) {
                erase_locations.insert(it->second.second);
                it = block_ranges.erase(it);
            } else {
                ++it;
            }
        }
    }
    return erase_locations;
}

template class BlockRangeInformation<u32>;
template class BlockRangeInformation<u64>;

}  // namespace Dynarmic::Backend
