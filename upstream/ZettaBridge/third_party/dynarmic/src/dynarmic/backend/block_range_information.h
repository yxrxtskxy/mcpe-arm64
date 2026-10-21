/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <map>
#include <utility>

#include <boost/icl/interval_set.hpp>
#include <tsl/robin_set.h>

#include "dynarmic/ir/location_descriptor.h"

namespace Dynarmic::Backend {

template<typename ProgramCounterType>
class BlockRangeInformation {
public:
    void AddRange(boost::icl::discrete_interval<ProgramCounterType> range, IR::LocationDescriptor location);
    void ClearCache();
    // The blocks that overlap the ranges. Their entries are removed: a block translated again
    // adds its range again.
    tsl::robin_set<IR::LocationDescriptor> InvalidateRanges(const boost::icl::interval_set<ProgramCounterType>& ranges);

private:
    // Each block by the first address it covers, with the last one. No block spans more than
    // max_span, so the blocks that overlap an address start at most that far below it. An
    // interval_map with a std::set per segment cost about 150 bytes of host memory per block.
    std::multimap<ProgramCounterType, std::pair<ProgramCounterType, IR::LocationDescriptor>> block_ranges;
    ProgramCounterType max_span = 0;
};

}  // namespace Dynarmic::Backend
