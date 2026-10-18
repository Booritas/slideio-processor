#include <slideio/processor/core/control.hpp>

#include <slideio/processor/core/error.hpp>

#include <atomic>
#include <utility>

namespace slideio::processor {

class CancellationToken::State {
public:
    std::atomic<bool> flag{false};
};

CancellationToken::CancellationToken() noexcept : state_() {}

CancellationToken::CancellationToken(std::shared_ptr<State> state) noexcept
    : state_(std::move(state)) {}

CancellationToken::CancellationToken(const CancellationToken& other) noexcept
    : state_(other.state_) {}

CancellationToken::CancellationToken(CancellationToken&& other) noexcept
    : state_(std::move(other.state_)) {
    other.state_.reset();
}

CancellationToken& CancellationToken::operator=(const CancellationToken& other) noexcept {
    state_ = other.state_;
    return *this;
}

CancellationToken& CancellationToken::operator=(CancellationToken&& other) noexcept {
    if (this != &other) {
        state_ = std::move(other.state_);
        other.state_.reset();
    }
    return *this;
}

CancellationToken::~CancellationToken() = default;

bool CancellationToken::isCancelled() const noexcept {
    return state_ && state_->flag.load(std::memory_order_acquire);
}

void CancellationToken::throwIfCancelled() const {
    if (isCancelled()) {
        throw CancelledError("operation cancelled");
    }
}

void CancellationToken::cancel() const noexcept {
    if (state_) {
        state_->flag.store(true, std::memory_order_release);
    }
}

CancellationSource::CancellationSource()
    : token_(std::make_shared<CancellationToken::State>()) {}

CancellationSource::~CancellationSource() = default;

CancellationToken CancellationSource::token() const noexcept {
    return token_;
}

void CancellationSource::cancel() noexcept {
    token_.cancel();
}

bool CancellationSource::isCancelled() const noexcept {
    return token_.isCancelled();
}

ProgressObserver::~ProgressObserver() = default;

} // namespace slideio::processor
