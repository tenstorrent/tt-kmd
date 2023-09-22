#include <iostream>

#include "util.h"
#include "enumeration.h"

void TestGetDeviceInfo(const EnumeratedDevice &dev);
void TestConfigSpace(const EnumeratedDevice &dev);
void TestQueryMappings(const EnumeratedDevice &dev);
void TestDmaBuf(const EnumeratedDevice &dev);
void TestPinPages(const EnumeratedDevice &dev);

int main(int argc, char *argv[])
{
    auto devs = EnumerateDevices();
    for (const auto &d : devs)
    {
        std::cout << "Testing " << d.path << " @ " << d.location.format() << '\n';

        TestGetDeviceInfo(d);
        TestConfigSpace(d);
        TestQueryMappings(d);
        TestDmaBuf(d);
        TestPinPages(d);
    }

    return 0;
}
