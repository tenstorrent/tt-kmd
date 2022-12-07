#include "test_failure.h"

std::string test_failure::format_msg(const std::string &msg, const char *file, unsigned int line, const char *func)
{
    return msg;
}

void test_failure::throw_new(const std::string &msg, const char *file, unsigned int line, const char *func)
{
    throw test_failure(msg, file, line, func);
}
