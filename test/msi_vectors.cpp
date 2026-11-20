// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

// Blackhole MSI vector routing. Vector 0 is firmware's and vector 1 carries
// HDMA completions. Firmware's SEND_PCIE_MSI message stands in for an HDMA
// completion: it raises the vector it is given, or vector 0 if the host did
// not enable that vector. So with 2 vectors an MSI on vector 1 must land only
// on vector 1, and with 1 vector (max_msi_vectors=1, or a platform without
// multi-message MSI) it must land on vector 0.

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include "ioctl.h"

#include "devfd.h"
#include "enumeration.h"
#include "test_failure.h"
#include "util.h"

namespace
{

constexpr std::uint32_t SMC_MSG_TYPE_SEND_PCIE_MSI = 0x17;

// NOC2AXI_CFG NOC_ID register of the PCIe tile, in BAR0: x is 2 for PCIe
// instance 0 and 11 for instance 1.
constexpr std::uint64_t NOC_ID_BAR0_OFFSET = 0x1FD00000 + 0x4044;
constexpr std::uint32_t PCIE_INST0_NOC_X = 2;
constexpr std::uint32_t PCIE_INST1_NOC_X = 11;

constexpr auto TIMEOUT = std::chrono::seconds(5);

std::uint16_t get_device_info_flags(DevFd &dev)
{
    tenstorrent_get_device_info info{};
    info.in.output_size_bytes = sizeof(info.out);

    if (ioctl(dev.get(), TENSTORRENT_IOCTL_GET_DEVICE_INFO, &info) != 0)
        THROW_TEST_FAILURE("TENSTORRENT_IOCTL_GET_DEVICE_INFO failed.");

    if (info.out.output_size_bytes < offsetof(tenstorrent_get_device_info_out, flags) + sizeof(info.out.flags))
        return 0;

    return info.out.flags;
}

std::uint64_t resource0_uc_base(DevFd &dev)
{
    constexpr unsigned int max_mappings = 16;
    std::vector<std::uint8_t> buf(sizeof(tenstorrent_query_mappings_in) + max_mappings * sizeof(tenstorrent_mapping));
    auto *query = reinterpret_cast<tenstorrent_query_mappings *>(buf.data());
    query->in.output_mapping_count = max_mappings;

    if (ioctl(dev.get(), TENSTORRENT_IOCTL_QUERY_MAPPINGS, query) != 0)
        THROW_TEST_FAILURE("TENSTORRENT_IOCTL_QUERY_MAPPINGS failed.");

    for (unsigned int i = 0; i < max_mappings; i++)
        if (query->out.mappings[i].mapping_id == TENSTORRENT_MAPPING_RESOURCE0_UC)
            return query->out.mappings[i].mapping_base;

    THROW_TEST_FAILURE("No RESOURCE0_UC mapping.");
}

// The PCIe instance (0 or 1) that this host is attached to.
std::uint32_t pcie_instance(DevFd &dev)
{
    std::uint64_t page_offset = NOC_ID_BAR0_OFFSET & ~std::uint64_t(page_size() - 1);
    std::uint64_t in_page = NOC_ID_BAR0_OFFSET - page_offset;

    void *p = mmap(nullptr, page_size(), PROT_READ, MAP_SHARED, dev.get(), resource0_uc_base(dev) + page_offset);
    if (p == MAP_FAILED)
        THROW_TEST_FAILURE("mmap of BAR0 failed.");

    std::uint32_t noc_x = *reinterpret_cast<volatile std::uint32_t *>(static_cast<std::uint8_t *>(p) + in_page) & 0x3F;
    munmap(p, page_size());

    if (noc_x == PCIE_INST0_NOC_X)
        return 0;
    if (noc_x == PCIE_INST1_NOC_X)
        return 1;

    THROW_TEST_FAILURE("Unexpected PCIe NOC x " + std::to_string(noc_x));
}

// MSI Multiple Message Capable as a vector count, or 0 if config space past
// the header cannot be read (it needs root).
unsigned int msi_vectors_capable(const EnumeratedDevice &dev)
{
    constexpr unsigned int capabilities_pointer = 0x34;
    constexpr std::uint8_t msi_cap_id = 0x05;
    constexpr unsigned int msi_message_control = 2;

    std::ifstream f(sysfs_dir_for_bdf(dev.location) + "/config", std::ios::binary);
    std::vector<std::uint8_t> config((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    if (config.size() < 256)
        return 0;

    for (unsigned int pos = config[capabilities_pointer] & 0xFC, hops = 0; pos != 0 && hops < 48;
         pos = config[pos + 1] & 0xFC, hops++) {
        if (config[pos] == msi_cap_id) {
            std::uint16_t control = config[pos + msi_message_control] | (config[pos + msi_message_control + 1] << 8);
            return 1u << ((control >> 1) & 0x7);
        }
    }

    THROW_TEST_FAILURE("MSI capability is missing.");
}

// IRQ numbers of the device's MSI vectors, in vector order.
std::vector<unsigned int> msi_irqs(const EnumeratedDevice &dev)
{
    std::vector<unsigned int> irqs;

    for (const auto &name : list_dir(sysfs_dir_for_bdf(dev.location) + "/msi_irqs"))
        irqs.push_back(std::stoul(name));

    std::sort(irqs.begin(), irqs.end());
    return irqs;
}

// Interrupt counts summed over all CPUs, by IRQ number.
std::map<unsigned int, std::uint64_t> interrupt_counts()
{
    std::map<unsigned int, std::uint64_t> counts;
    std::ifstream f("/proc/interrupts");
    std::string line;

    std::getline(f, line); // CPU header
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string irq_label;
        ss >> irq_label;

        if (irq_label.empty() || irq_label.back() != ':')
            continue;

        unsigned int irq;
        try {
            irq = std::stoul(irq_label);
        } catch (const std::exception &) {
            continue; // NMI:, LOC:, ...
        }

        std::uint64_t total = 0;
        std::string field;
        auto is_count = [](const std::string &s) {
            return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
        };
        while (ss >> field && is_count(field))
            total += std::stoull(field);

        counts[irq] = total;
    }

    return counts;
}

void send_msi(DevFd &dev, std::uint32_t pcie_inst, std::uint32_t vector)
{
    tenstorrent_smc_msg msg{};
    msg.argsz = sizeof(msg);
    msg.flags = TENSTORRENT_SMC_MSG_POST;
    msg.message[0] = SMC_MSG_TYPE_SEND_PCIE_MSI | (pcie_inst << 8);
    msg.message[1] = vector;

    if (ioctl(dev.get(), TENSTORRENT_IOCTL_SMC_MSG, &msg) != 0)
        THROW_TEST_FAILURE("SMC_MSG POST of SEND_PCIE_MSI failed.");

    auto deadline = std::chrono::steady_clock::now() + TIMEOUT;
    for (;;) {
        msg = {};
        msg.argsz = sizeof(msg);
        msg.flags = TENSTORRENT_SMC_MSG_POLL;

        if (ioctl(dev.get(), TENSTORRENT_IOCTL_SMC_MSG, &msg) == 0)
            break;
        if (errno != EAGAIN)
            THROW_TEST_FAILURE("SMC_MSG POLL of SEND_PCIE_MSI failed.");
        if (std::chrono::steady_clock::now() >= deadline)
            THROW_TEST_FAILURE("Timed out waiting for SEND_PCIE_MSI.");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if ((msg.message[0] & 0xFF) != 0)
        THROW_TEST_FAILURE("SEND_PCIE_MSI returned status " + std::to_string(msg.message[0] & 0xFF));
}

// Waits for the interrupt on want_irq. Returns the count deltas of all irqs.
std::map<unsigned int, std::uint64_t> wait_for_interrupt(const std::vector<unsigned int> &irqs,
                                                         const std::map<unsigned int, std::uint64_t> &before,
                                                         unsigned int want_irq)
{
    auto deadline = std::chrono::steady_clock::now() + TIMEOUT;

    for (;;) {
        auto now_counts = interrupt_counts();
        std::map<unsigned int, std::uint64_t> delta;

        for (auto irq : irqs)
            delta[irq] = now_counts[irq] - before.at(irq);

        if (delta[want_irq] > 0 || std::chrono::steady_clock::now() >= deadline)
            return delta;

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

} // namespace

void TestMsiVectors(const EnumeratedDevice &dev)
{
    if (dev.type != Blackhole)
        return;

    DevFd dev_fd(dev.path);

    // Without the firmware capability the driver reports no HDMA, and firmware
    // may still drop an MSI on a vector the host did not enable.
    if (!(get_device_info_flags(dev_fd) & TENSTORRENT_DEVICE_INFO_HDMA)) {
        std::cout << "Skipping MSI vector test on " << dev.path << ": no HDMA support reported.\n";
        return;
    }

    // Firmware with the capability advertises 2 vectors, also after a reset.
    unsigned int capable = msi_vectors_capable(dev);
    if (capable == 0)
        std::cout << "Cannot read config space; skipping the MSI capability check.\n";
    else if (capable != 2)
        THROW_TEST_FAILURE("MSI advertises " + std::to_string(capable) + " vectors, expected 2.");

    auto irqs = msi_irqs(dev);
    if (irqs.empty() || irqs.size() > 2)
        THROW_TEST_FAILURE("Expected 1 or 2 MSI vectors, got " + std::to_string(irqs.size()));

    if (irqs.size() == 1)
        std::cout << "Note: " << dev.path << " has 1 MSI vector; HDMA completions share vector 0.\n";

    std::uint32_t inst = pcie_instance(dev_fd);
    unsigned int hdma_irq = irqs.size() == 2 ? irqs[1] : irqs[0];

    // An HDMA-vector MSI lands on the HDMA vector, or on vector 0 without one.
    auto before = interrupt_counts();
    send_msi(dev_fd, inst, 1);
    auto delta = wait_for_interrupt(irqs, before, hdma_irq);

    if (delta[hdma_irq] == 0)
        THROW_TEST_FAILURE("MSI on vector 1 did not arrive on IRQ " + std::to_string(hdma_irq));

    if (irqs.size() == 1)
        return;

    // Firmware only raises vector 0 on its own, so vector 1 stays quiet when
    // vector 0 fires.
    before = interrupt_counts();
    send_msi(dev_fd, inst, 0);
    delta = wait_for_interrupt(irqs, before, irqs[0]);

    if (delta[irqs[0]] == 0)
        THROW_TEST_FAILURE("MSI on vector 0 did not arrive on IRQ " + std::to_string(irqs[0]));

    if (delta[irqs[1]] != 0)
        THROW_TEST_FAILURE("MSI on vector 0 also arrived on the HDMA vector.");
}
