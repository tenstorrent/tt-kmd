#include "util.h"
#include "enumeration.h"

void TestGetDeviceInfo(const EnumeratedDevice &dev);
void TestQueryMappings(const EnumeratedDevice &dev);
void TestDmaBuf(const EnumeratedDevice &dev);
void TestPinPages(const EnumeratedDevice &dev);

int main(int argc, char *argv[])
{
    auto devs = EnumerateDevices();
    for (const auto &d : devs)
    {
        TestGetDeviceInfo(d);
        TestQueryMappings(d);
        TestDmaBuf(d);
        TestPinPages(d);
    }

    return 0;
}
