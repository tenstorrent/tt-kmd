// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <filesystem>
#include <map>

#include "enumeration.h"
#include "test_failure.h"

// Ensure that the labels in the hwmon directory are as expected.
static void VerifyLabels(const std::filesystem::path &hwmon_dir)
{
    // Map filename to expected contents of the file.
    static const std::map<std::string, std::string> labels = {
        { "curr1_label",    "current" },
        { "in0_label",      "vcore" },
        { "temp1_label",    "asic_temp" }
    };

    for (const auto &label : labels) {
        const auto filename = label.first;
        const auto expected = label.second;
        auto actual = read_file(hwmon_dir / filename);

        actual.pop_back(); // Remove the newline at the end of the file.

        if (actual != expected)
            THROW_TEST_FAILURE(hwmon_dir.string() + "/" + filename + " contains " + actual + ", expected " + expected);
    }
}

static void VerifyInputsAreUnderMaxes(const std::filesystem::path &hwmon_dir)
{
    // Map sensor input filename to max filename.
    static const std::map<std::string, std::string> inputs_to_maxes = {
        { "in0_input", "in0_max" },
        { "curr1_input", "curr1_max" },
        { "temp1_input", "temp1_max" }
    };

    for (const auto &item : inputs_to_maxes) {
        const auto input_filename = item.first;
        const auto max_filename = item.second;

        auto input = read_file(hwmon_dir / input_filename);
        auto max = read_file(hwmon_dir / max_filename);

        // Remove newlines.
        input.pop_back();
        max.pop_back();

        int32_t numeric_input;
        int32_t numeric_max;

        try {
            numeric_input = std::stoi(input);
            numeric_max = std::stoi(max);
        } catch (const std::exception &e) {
            THROW_TEST_FAILURE("Failed to convert " + input + " or " + max + " to an integer.");
        }

        if (numeric_input >= numeric_max)
            THROW_TEST_FAILURE(hwmon_dir.string() + "/" + input_filename + " is " + input + ", but " + max_filename + " is " + max);
    }
}

// If 0000:01:00.0 is a Tenstorrent AI accelerator with hwmon enabled, then a
// /sys/bus/pci/devices/0000:01:00.0/hwmon/hwmonX directory should exist, and it
// will contain files exposing sensor data.
void TestHwmon(const EnumeratedDevice &dev)
{
    std::filesystem::path sysfs_dir = sysfs_dir_for_bdf(dev.location);
    std::filesystem::path hwmon_dir = sysfs_dir / "hwmon";

    if (!std::filesystem::exists(hwmon_dir))
        return; // No hwmon directory, nothing to test.

    for (const auto &entry : std::filesystem::directory_iterator(hwmon_dir))
    {
        if (entry.is_directory() && entry.path().filename().string().find("hwmon") != std::string::npos) {
            // Found the hwmonX directory.
            const auto target_dir = entry.path();

            VerifyLabels(target_dir);
            VerifyInputsAreUnderMaxes(target_dir);
        }
    }
}
