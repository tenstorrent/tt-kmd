// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

// Verify that all resource IDs are known to us.
// Verify that all the UNUSEDs are at the end of the output array.
// Verify that no non-UNUSED mapping ID appears more than once.
// Verify that RESOURCE0_UC appears in the results.
// Verify that if RESOURCEi_WC appears, then RESOURCEi_UC also appears.
// Verify that there's no overlap in the base/size. Verify that size > 0. Verify that base & size are multiples of the page size.
// Verify that not giving enough space for outputs results in the initial subset being returned.

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <cstddef>
#include <cstdint>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include "ioctl.h"

#include "util.h"
#include "test_failure.h"
#include "enumeration.h"
#include "devfd.h"
#include "aligned_allocator.h"

typedef std::uint32_t mapping_id;

void VerifyKnownIds(const std::vector<tenstorrent_mapping> &mappings)
{
    static const mapping_id known_mapping_ids[] =
    {
        TENSTORRENT_MAPPING_UNUSED,
        TENSTORRENT_MAPPING_RESOURCE0_UC,
        TENSTORRENT_MAPPING_RESOURCE0_WC,
        TENSTORRENT_MAPPING_RESOURCE1_UC,
        TENSTORRENT_MAPPING_RESOURCE1_WC,
        TENSTORRENT_MAPPING_RESOURCE2_UC,
        TENSTORRENT_MAPPING_RESOURCE2_WC,
    };

    for (const auto &mapping : mappings)
    {
        if (std::find(std::begin(known_mapping_ids), std::end(known_mapping_ids), mapping.mapping_id) == std::end(known_mapping_ids))
            THROW_TEST_FAILURE("Found unknown mapping ID in QUERY_MAPPINGS.");
    }
}

void VerifyUnusedAtEnd(const std::vector<tenstorrent_mapping> &mappings)
{
    auto first_unused = std::find_if(mappings.begin(), mappings.end(),
                                     [](auto &&m) { return m.mapping_id == TENSTORRENT_MAPPING_UNUSED; });

    if (std::find_if(first_unused, mappings.end(),
                     [](auto &&m) { return m.mapping_id != TENSTORRENT_MAPPING_UNUSED; }) != mappings.end())
        THROW_TEST_FAILURE("Found unused mapping in the middle of QUERY_MAPPINGS results.");
}

void VerifyUniqueMappingIds(const std::vector<tenstorrent_mapping> &mappings)
{
    std::set<mapping_id> ids;
    for (const auto &m : mappings)
    {
        if (m.mapping_id != TENSTORRENT_MAPPING_UNUSED && !ids.insert(m.mapping_id).second)
            THROW_TEST_FAILURE("Duplicated mapping id in QUERY_MAPPINGS results.");
    }
}

void VerifyResource0UCPresent(const std::vector<tenstorrent_mapping> &mappings)
{
    if (std::none_of(mappings.begin(), mappings.end(), [](auto &&m) { return m.mapping_id == TENSTORRENT_MAPPING_RESOURCE0_UC; }))
        THROW_TEST_FAILURE("No mapping for resource 0 UC.");
}

void VerifyResourceWCUCPresent(const std::vector<tenstorrent_mapping> &mappings)
{
    static const std::tuple<mapping_id, mapping_id> wc_uc[] =
    {
        { TENSTORRENT_MAPPING_RESOURCE0_WC, TENSTORRENT_MAPPING_RESOURCE0_UC },
        { TENSTORRENT_MAPPING_RESOURCE1_WC, TENSTORRENT_MAPPING_RESOURCE1_UC },
        { TENSTORRENT_MAPPING_RESOURCE2_WC, TENSTORRENT_MAPPING_RESOURCE2_UC },
    };

    for (const auto &uw : wc_uc)
    {
        bool wc_present = std::any_of(mappings.begin(), mappings.end(),
                                      [&] (const auto &m) { return m.mapping_id == std::get<0>(uw); });

        bool uc_present = std::any_of(mappings.begin(), mappings.end(),
                                      [&] (const auto &m) { return m.mapping_id == std::get<1>(uw); });

        if (wc_present && !uc_present)
            THROW_TEST_FAILURE("Found WC mapping for a resource with matching UC mapping.");
    }
}

// Verify that there's no overlap in the base/size.
void VerifyNoOverlap(const std::vector<tenstorrent_mapping> &mappings)
{
    if (mappings.empty())
        return;

    auto base_compare = [](const tenstorrent_mapping &l, const tenstorrent_mapping &r)
    {
        return l.mapping_base < r.mapping_base;
    };

    std::vector<tenstorrent_mapping> base_sorted_mappings;
    std::copy_if(mappings.begin(), mappings.end(), std::back_inserter(base_sorted_mappings),
                 [] (const auto &m) { return m.mapping_id != TENSTORRENT_MAPPING_UNUSED; });

    std::sort(base_sorted_mappings.begin(), base_sorted_mappings.end(), base_compare);

    for (unsigned int i = 0; i < mappings.size()-1; i++)
    {
        if (mappings[i].mapping_size > mappings[i+1].mapping_base - mappings[i].mapping_base)
            THROW_TEST_FAILURE("Found overlapping mappings in QUERY_MAPPINGS results.");
    }

    // All values are unsigned ints - wraparound assured.
    if (mappings.back().mapping_base + mappings.back().mapping_size < mappings.back().mapping_base)
        THROW_TEST_FAILURE("Mapping is so large that it wraps around.");
}

// Verify that size > 0. Verify that base & size are multiples of the page size.
// Verify that the size is not too large and that mapping_base is not too high.
void VerifySizes(const std::vector<tenstorrent_mapping> &mappings)
{
    if (std::any_of(mappings.begin(), mappings.end(),
                    [](const auto &m) { return m.mapping_id != TENSTORRENT_MAPPING_UNUSED && m.mapping_size == 0; }))
        THROW_TEST_FAILURE("Zero-size mapping in QUERY_MAPPINGS results.");

    auto pagesize = page_size();

    if (std::any_of(mappings.begin(), mappings.end(),
                    [=](const auto &m) { return m.mapping_id != TENSTORRENT_MAPPING_UNUSED && m.mapping_size % pagesize != 0; }))
        THROW_TEST_FAILURE("Mapping size is not a multiple of page size in QUERY_MAPPINGS results.");

    if (std::any_of(mappings.begin(), mappings.end(),
                    [=](const auto &m) { return m.mapping_id != TENSTORRENT_MAPPING_UNUSED && m.mapping_base % pagesize != 0; }))
        THROW_TEST_FAILURE("Mapping base is not a multiple of page size in QUERY_MAPPINGS results.");

    if (std::any_of(mappings.begin(), mappings.end(),
                    [](const auto &m) { return m.mapping_size > std::numeric_limits<std::uint64_t>::max() - m.mapping_base; }))
        THROW_TEST_FAILURE("Mapping region wraps around.");

    std::uint64_t mmap_offset_limit_for_32b = (std::uint64_t)1 << 44; // 32 + log(PAGE_SIZE)

    if (std::any_of(mappings.begin(), mappings.end(),
                    [=](const auto &m) { return m.mapping_size + m.mapping_base >= mmap_offset_limit_for_32b; }))
        THROW_TEST_FAILURE("Mapping base/size do not fit into 32-bit mmap offset.");
}

void PrintMappings(const std::vector<tenstorrent_mapping>& mappings)
{
    static const char *names[] = {
        [TENSTORRENT_MAPPING_UNUSED] = "TENSTORRENT_MAPPING_UNUSED",
        [TENSTORRENT_MAPPING_RESOURCE0_UC] = "TENSTORRENT_MAPPING_RESOURCE0_UC",
        [TENSTORRENT_MAPPING_RESOURCE0_WC] = "TENSTORRENT_MAPPING_RESOURCE0_WC",
        [TENSTORRENT_MAPPING_RESOURCE1_UC] = "TENSTORRENT_MAPPING_RESOURCE1_UC",
        [TENSTORRENT_MAPPING_RESOURCE1_WC] = "TENSTORRENT_MAPPING_RESOURCE1_WC",
        [TENSTORRENT_MAPPING_RESOURCE2_UC] = "TENSTORRENT_MAPPING_RESOURCE2_UC",
        [TENSTORRENT_MAPPING_RESOURCE2_WC] = "TENSTORRENT_MAPPING_RESOURCE2_WC",
    };

    for (const tenstorrent_mapping &m : mappings)
    {
        const char *name = "unknown";
        if (m.mapping_id < sizeof(names) / sizeof(names[0]))
            name = names[m.mapping_id];

        std::cout << m.mapping_id << ' ' << name << ' ' << std::hex << m.mapping_base << '+' << m.mapping_size << std::dec << '\n';
    }
}

std::vector<tenstorrent_mapping> QueryMappingsCount(int dev_fd, std::uint32_t count)
{
    std::vector<std::byte, AlignedAllocator<std::byte, alignof(tenstorrent_query_mappings)>> buf;
    buf.resize(sizeof(tenstorrent_query_mappings) + count * sizeof(tenstorrent_mapping));

    tenstorrent_query_mappings query;
    zero(&query);
    query.in.output_mapping_count = count;
    std::memcpy(buf.data(), &query, sizeof(query));

    if (ioctl(dev_fd, TENSTORRENT_IOCTL_QUERY_MAPPINGS, buf.data()) != 0)
        THROW_TEST_FAILURE("TENSTORRENT_IOCTL_QUERY_MAPPINGS failed.");

    std::vector<tenstorrent_mapping> results(count);
    std::memcpy(results.data(), buf.data() + offsetof(tenstorrent_query_mappings, out.mappings), count * sizeof(tenstorrent_mapping));

    return results;
}

std::vector<tenstorrent_mapping> QueryMappings(int dev_fd)
{
    std::uint32_t count = 16;

    while (true)
    {
        auto mappings = QueryMappingsCount(dev_fd, count);
        if (mappings.back().mapping_id == TENSTORRENT_MAPPING_UNUSED)
            return mappings;

        count *= 2;
    }
}

void VerifyPrefixes(int dev_fd, const std::vector<tenstorrent_mapping> &mappings)
{
    for (unsigned int i = 0; i < mappings.size(); i++)
    {
        auto prefix = QueryMappingsCount(dev_fd, i);

        for (unsigned int j = 0; j < prefix.size(); j++)
        {
            if (prefix[j].mapping_id != mappings[j].mapping_id
                || prefix[j].mapping_base != mappings[j].mapping_base
                || prefix[j].mapping_size != mappings[j].mapping_size)
                THROW_TEST_FAILURE("QUERY_MAPPINGS prefix is inconsistent with full result.");
        }
    }
}

void VerifyMmap(int dev_fd, const std::vector<tenstorrent_mapping> &mappings)
{
    for (const auto &m : mappings)
    {
        if (m.mapping_id != TENSTORRENT_MAPPING_UNUSED)
        {
            void *p = mmap(nullptr, m.mapping_size, PROT_READ | PROT_WRITE, MAP_SHARED, dev_fd, m.mapping_base);
            if (p == MAP_FAILED)
                THROW_TEST_FAILURE("mmap of a mapping failed.");

            if (munmap(p, m.mapping_size) == -1)
                THROW_TEST_FAILURE("munmap of a mapping failed.");
        }
    }
}

void TestQueryMappings(const EnumeratedDevice &dev)
{
    DevFd dev_fd(dev.path);

    auto mappings = QueryMappings(dev_fd.get());

    VerifyKnownIds(mappings);
    VerifyUnusedAtEnd(mappings);
    VerifyUniqueMappingIds(mappings);
    VerifyResource0UCPresent(mappings);
    VerifyResourceWCUCPresent(mappings);
    VerifyNoOverlap(mappings);
    VerifySizes(mappings);
    VerifyPrefixes(dev_fd.get(), mappings);
    VerifyMmap(dev_fd.get(), mappings);
}
