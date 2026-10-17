#include <slideio/processor/core/channel_selection.hpp>

#include <slideio/processor/core/error.hpp>

#include <algorithm>
#include <climits>
#include <utility>
#include <vector>

namespace slideio::processor {

class ChannelSelection::Impl {
public:
    std::vector<int32_t> channels;
};

ChannelSelection::ChannelSelection() : impl_(), all_(false) {}

ChannelSelection::ChannelSelection(const ChannelSelection& other)
    : impl_(other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr), all_(other.all_) {}

ChannelSelection::ChannelSelection(ChannelSelection&& other) noexcept
    : impl_(std::move(other.impl_)), all_(other.all_) {
    other.impl_.reset();
    other.all_ = false;
}

ChannelSelection& ChannelSelection::operator=(const ChannelSelection& other) {
    if (this != &other) {
        auto copy = other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr;
        impl_ = std::move(copy);
        all_ = other.all_;
    }
    return *this;
}

ChannelSelection& ChannelSelection::operator=(ChannelSelection&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_);
        all_ = other.all_;
        other.impl_.reset();
        other.all_ = false;
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
    if (static_cast<int64_t>(first) + count - 1 > INT32_MAX) {
        throw InvalidArgumentError("ChannelSelection::range: first + count - 1 exceeds INT32_MAX");
    }
    ChannelSelection s;
    s.impl_ = std::make_unique<Impl>();
    s.impl_->channels.reserve(static_cast<size_t>(count));
    for (int64_t i = 0; i < count; ++i) {
        s.impl_->channels.push_back(static_cast<int32_t>(first + i));
    }
    return s;
}

ChannelSelection ChannelSelection::all() noexcept {
    ChannelSelection s;
    s.all_ = true;
    return s;
}

ChannelSelection& ChannelSelection::add(int32_t channel) {
    if (channel < 0) {
        throw InvalidArgumentError("ChannelSelection::add: channel must be >= 0");
    }
    if (all_) {
        throw InvalidArgumentError("ChannelSelection::add: the selection is all channels");
    }
    if (contains(channel)) {
        throw InvalidArgumentError("ChannelSelection::add: channel already selected");
    }
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
    impl_->channels.push_back(channel);
    return *this;
}

bool ChannelSelection::isAll() const noexcept { return all_; }

int32_t ChannelSelection::count() const noexcept {
    return impl_ ? static_cast<int32_t>(impl_->channels.size()) : 0;
}

int32_t ChannelSelection::at(int32_t index) const {
    if (all_ || !impl_ || index < 0 || index >= static_cast<int32_t>(impl_->channels.size())) {
        throw InvalidArgumentError("ChannelSelection::at: index out of range");
    }
    return impl_->channels[static_cast<size_t>(index)];
}

bool ChannelSelection::contains(int32_t channel) const noexcept {
    if (channel < 0) {
        return false;
    }
    if (all_) {
        return true;
    }
    return impl_ && std::find(impl_->channels.begin(), impl_->channels.end(), channel) != impl_->channels.end();
}

} // namespace slideio::processor
