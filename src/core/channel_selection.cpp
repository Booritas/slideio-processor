#include <slideio/processor/core/channel_selection.hpp>

#include <slideio/processor/core/error.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace slideio::processor {

class ChannelSelection::Impl {
public:
    bool all = false;
    std::vector<int32_t> channels;
};

ChannelSelection::ChannelSelection() : impl_(std::make_unique<Impl>()) {}

ChannelSelection::ChannelSelection(const ChannelSelection& other)
    : impl_(other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr) {}

ChannelSelection::ChannelSelection(ChannelSelection&& other) noexcept : impl_(std::move(other.impl_)) {}

ChannelSelection& ChannelSelection::operator=(const ChannelSelection& other) {
    if (this != &other) {
        impl_ = other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr;
    }
    return *this;
}

ChannelSelection& ChannelSelection::operator=(ChannelSelection&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_);
    }
    return *this;
}

ChannelSelection::~ChannelSelection() = default;

ChannelSelection ChannelSelection::single(int32_t channel) {
    ChannelSelection s;
    s.add(channel);
    return s;
}

ChannelSelection ChannelSelection::range(int32_t first, int32_t count) {
    if (first < 0 || count < 1) {
        throw InvalidArgumentError("ChannelSelection::range: first must be >= 0 and count >= 1");
    }
    ChannelSelection s;
    for (int64_t i = 0; i < count; ++i) {
        s.add(static_cast<int32_t>(first + i));
    }
    return s;
}

ChannelSelection ChannelSelection::all() {
    ChannelSelection s;
    s.impl_->all = true;
    return s;
}

void ChannelSelection::add(int32_t channel) {
    if (channel < 0) {
        throw InvalidArgumentError("ChannelSelection::add: channel must be >= 0");
    }
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
    if (impl_->all) {
        throw InvalidArgumentError("ChannelSelection::add: the selection is all channels");
    }
    auto& v = impl_->channels;
    if (std::find(v.begin(), v.end(), channel) != v.end()) {
        throw InvalidArgumentError("ChannelSelection::add: channel already selected");
    }
    v.push_back(channel);
}

bool ChannelSelection::isAll() const noexcept { return impl_ && impl_->all; }

int32_t ChannelSelection::count() const noexcept {
    return impl_ ? static_cast<int32_t>(impl_->channels.size()) : 0;
}

int32_t ChannelSelection::at(int32_t index) const {
    if (!impl_ || impl_->all || index < 0 || index >= static_cast<int32_t>(impl_->channels.size())) {
        throw InvalidArgumentError("ChannelSelection::at: index out of range");
    }
    return impl_->channels[static_cast<size_t>(index)];
}

bool ChannelSelection::contains(int32_t channel) const noexcept {
    if (!impl_ || channel < 0) {
        return false;
    }
    if (impl_->all) {
        return true;
    }
    return std::find(impl_->channels.begin(), impl_->channels.end(), channel) != impl_->channels.end();
}

} // namespace slideio::processor
