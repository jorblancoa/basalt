#include <gsl-lite/gsl-lite.hpp>

#include "py_helpers.hpp"

namespace basalt {

membuf::~membuf() = default;

membuf::membuf(pybind11::array_t<char>& data) {
    auto request = data.request();
    auto ptr = static_cast<char*>(request.ptr);
    gsl::span<char> span(ptr, static_cast<std::size_t>(request.size));
    this->setg(ptr, span.begin(), span.end());
}

membuf::membuf(char* base, size_t size) {
    gsl::span<char> span(base, size);
    this->setg(base, base, span.end());
}

imemstream::~imemstream() = default;

imemstream::imemstream(char* base, size_t size)
    : membuf(base, size)
    , std::istream(this) {}

imemstream::imemstream(pybind11::array_t<char>& data)
    : membuf(data)
    , std::istream(this) {}

}  // namespace basalt