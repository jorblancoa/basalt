#include <basalt/vertex_iterator.hpp>
#include <basalt/vertices.hpp>

#include "graph_impl.hpp"
#include "vertex_iterator_impl.hpp"

namespace basalt {

VertexIterator::VertexIterator(const basalt::GraphImpl& pimpl, size_t from) {
    // \fixme TCL this limit value is VertexIteratorImpl internal stuff
    // and so should not be used here
    if (from == std::numeric_limits<std::size_t>::max()) {
        pimpl_ = VertexIteratorImpl_ptr(nullptr);
    } else {
        pimpl_ = pimpl.VertexIterator(from);
        std::advance(*this, static_cast<VertexIterator::difference_type>(from));
    }
}

VertexIterator::VertexIterator(const basalt::VertexIterator& other)
    : pimpl_(other.pimpl_) {}

VertexIterator& VertexIterator::operator++() {
    ++*pimpl_;
    return *this;
}

const VertexIterator VertexIterator::operator++(int value) {
    const VertexIterator result(*this);
    while (value-- > 0) {
        this->operator++();
    }
    return result;
}

bool VertexIterator::operator==(const basalt::VertexIterator& other) const {
    if (pimpl_) {
        if (other.pimpl_) {
            return *this->pimpl_ == *other.pimpl_;
        }
        return pimpl_->end_reached();
    }
    return !other.pimpl_;
}

bool VertexIterator::operator!=(const basalt::VertexIterator& other) const {
    return !(*this == other);
}

const VertexIterator::value_type& VertexIterator::operator*() {
    return **pimpl_;
}

}  // namespace basalt