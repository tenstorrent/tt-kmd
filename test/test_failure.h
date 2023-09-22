#pragma once

#include <stdexcept>
#include <string>

#define THROW_TEST_FAILURE(msg) test_failure::throw_new((msg), __FILE__, __LINE__, __func__)

class test_failure : public std::runtime_error
{
public:
    test_failure(const std::string &msg, const char *file, unsigned int line, const char *func)
        : std::runtime_error(format_msg(msg, file, line, func)),
          file(file), line(line), func(func)
          {}

    [[noreturn]] static void throw_new(const std::string &msg, const char *file, unsigned int line, const char *func);

private:
    const char *file;
    unsigned int line;
    const char *func;

    static std::string format_msg(const std::string &msg, const char *file, unsigned int line, const char *func);
};
