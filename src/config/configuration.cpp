#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/error.hpp>

#include <atomic>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace slideio::processor {

namespace {

// The ten settings, shared by Configuration and ConfigurationBuilder.
struct Settings {
    int64_t memoryCeilingBytes = int64_t(2) << 30;
    int32_t workerCount = defaultWorkerCount();
    int32_t tileWidth = 1024;
    int32_t tileHeight = 1024;
    bool tileSizeFromSlide = true;
    std::string logFilePath;
    LogLevel logLevel = LogLevel::Info;
    int32_t eventPollIntervalMs = 150;
    bool eventSignalEnabled = true;
    int64_t objectBatchSize = 512;
    std::string scratchDirectory;

    // Read once per process: a syscall on some platforms, and a value that must
    // not differ between two default objects if the affinity mask changes.
    static int32_t defaultWorkerCount() {
        static const int32_t count = [] {
            const unsigned hardware = std::thread::hardware_concurrency();
            return hardware == 0 ? 1 : static_cast<int32_t>(hardware);
        }();
        return count;
    }
};

// What a moved-from object reads: its impl is null, so a move never allocates.
const Settings& defaultSettings() noexcept {
    static const Settings settings;
    return settings;
}

// The settings an impl reads as: its own, or the defaults when it is null.
template <class Impl>
const Settings& settingsOf(const std::unique_ptr<Impl>& impl) noexcept {
    return impl ? impl->settings : defaultSettings();
}

// A copy of an impl; a null (moved-from) source copies as default-constructed.
template <class Impl>
std::unique_ptr<Impl> copyOf(const std::unique_ptr<Impl>& impl) {
    return impl ? std::make_unique<Impl>(*impl) : std::make_unique<Impl>();
}

// The impl to modify, recreated on the first write after a move.
template <class Impl>
Impl& writable(std::unique_ptr<Impl>& impl) {
    if (!impl)
        impl = std::make_unique<Impl>();
    return *impl;
}

bool equal(const Settings& a, const Settings& b) {
    return a.memoryCeilingBytes == b.memoryCeilingBytes && a.workerCount == b.workerCount &&
           a.tileWidth == b.tileWidth && a.tileHeight == b.tileHeight &&
           a.tileSizeFromSlide == b.tileSizeFromSlide && a.logFilePath == b.logFilePath &&
           a.logLevel == b.logLevel && a.eventPollIntervalMs == b.eventPollIntervalMs &&
           a.eventSignalEnabled == b.eventSignalEnabled && a.objectBatchSize == b.objectBatchSize &&
           a.scratchDirectory == b.scratchDirectory;
}

bool logLevelInRange(LogLevel level) {
    const int32_t value = static_cast<int32_t>(level);
    return value >= static_cast<int32_t>(LogLevel::Trace) && value <= static_cast<int32_t>(LogLevel::Off);
}

// The process default: every published instance is kept until process exit, so
// a reference handed out earlier never dangles. A function-local static avoids
// any initialisation-order dependence. Readers load `current` without the lock;
// the mutex serialises publishers only.
struct ProcessDefaultRegistry {
    std::mutex mutex;
    std::vector<std::unique_ptr<Configuration>> published;
    std::atomic<const Configuration*> current{nullptr};
};

ProcessDefaultRegistry& registry() {
    // Deliberately leaked (G3: "never destroyed before process exit"): a static
    // object would be destroyed at exit, dangling references still held by
    // other static destructors.
    // The initial default is published here, once, so processDefault() itself
    // never allocates.
    static auto* instance = [] {
        auto* r = new ProcessDefaultRegistry;
        r->published.push_back(std::make_unique<Configuration>());
        r->current.store(r->published.back().get(), std::memory_order_release);
        return r;
    }();
    return *instance;
}

} // namespace

class Configuration::Impl {
public:
    Settings settings;
};

Configuration::Configuration() : impl_(std::make_unique<Impl>()) {}

Configuration::Configuration(const Configuration& other) : impl_(copyOf(other.impl_)) {}

Configuration::Configuration(Configuration&& other) noexcept : impl_(std::move(other.impl_)) {}

Configuration& Configuration::operator=(const Configuration& other) {
    if (this != &other)
        impl_ = copyOf(other.impl_);
    return *this;
}

Configuration& Configuration::operator=(Configuration&& other) noexcept {
    if (this != &other)
        impl_ = std::move(other.impl_);
    return *this;
}

Configuration::~Configuration() = default;

Configuration Configuration::defaults() noexcept {
    return Configuration();
}

const Configuration& Configuration::processDefault() noexcept {
    return *registry().current.load(std::memory_order_acquire);
}

void Configuration::setProcessDefault(const Configuration& configuration) {
    try {
        auto next = std::make_unique<Configuration>(configuration);
        ProcessDefaultRegistry& r = registry();
        std::lock_guard<std::mutex> lock(r.mutex); // may throw std::system_error
        r.published.push_back(std::move(next));
        r.current.store(r.published.back().get(), std::memory_order_release);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) {
        throw InternalError(e.what());
    }
}

int64_t Configuration::memoryCeilingBytes() const noexcept {
    return settingsOf(impl_).memoryCeilingBytes;
}
int32_t Configuration::workerCount() const noexcept {
    return settingsOf(impl_).workerCount;
}
int32_t Configuration::tileWidth() const noexcept {
    return settingsOf(impl_).tileWidth;
}
int32_t Configuration::tileHeight() const noexcept {
    return settingsOf(impl_).tileHeight;
}
bool Configuration::tileSizeFromSlide() const noexcept {
    return settingsOf(impl_).tileSizeFromSlide;
}
const char* Configuration::logFilePath() const noexcept {
    return settingsOf(impl_).logFilePath.c_str();
}
LogLevel Configuration::logLevel() const noexcept {
    return settingsOf(impl_).logLevel;
}
int32_t Configuration::eventPollIntervalMs() const noexcept {
    return settingsOf(impl_).eventPollIntervalMs;
}
bool Configuration::eventSignalEnabled() const noexcept {
    return settingsOf(impl_).eventSignalEnabled;
}
int64_t Configuration::objectBatchSize() const noexcept {
    return settingsOf(impl_).objectBatchSize;
}
const char* Configuration::scratchDirectory() const noexcept {
    return settingsOf(impl_).scratchDirectory.c_str();
}

bool Configuration::equals(const Configuration& other) const noexcept {
    return equal(settingsOf(impl_), settingsOf(other.impl_));
}

class ConfigurationBuilder::Impl {
public:
    Settings settings;
    // A null string is recorded here and reported by build() (setters never validate).
    bool logFilePathNull = false;
    bool scratchDirectoryNull = false;
};

ConfigurationBuilder::ConfigurationBuilder() : impl_(std::make_unique<Impl>()) {}

ConfigurationBuilder::ConfigurationBuilder(const Configuration& from) : impl_(std::make_unique<Impl>()) {
    impl_->settings = settingsOf(from.impl_);
}

ConfigurationBuilder::ConfigurationBuilder(const ConfigurationBuilder& other) : impl_(copyOf(other.impl_)) {}

ConfigurationBuilder::ConfigurationBuilder(ConfigurationBuilder&& other) noexcept
    : impl_(std::move(other.impl_)) {}

ConfigurationBuilder& ConfigurationBuilder::operator=(const ConfigurationBuilder& other) {
    if (this != &other)
        impl_ = copyOf(other.impl_);
    return *this;
}

ConfigurationBuilder& ConfigurationBuilder::operator=(ConfigurationBuilder&& other) noexcept {
    if (this != &other)
        impl_ = std::move(other.impl_);
    return *this;
}

ConfigurationBuilder::~ConfigurationBuilder() = default;

ConfigurationBuilder& ConfigurationBuilder::memoryCeilingBytes(int64_t bytes) {
    writable(impl_).settings.memoryCeilingBytes = bytes;
    return *this;
}

ConfigurationBuilder& ConfigurationBuilder::workerCount(int32_t count) {
    writable(impl_).settings.workerCount = count;
    return *this;
}

ConfigurationBuilder& ConfigurationBuilder::tileSize(int32_t width, int32_t height) {
    Settings& settings = writable(impl_).settings;
    settings.tileWidth = width;
    settings.tileHeight = height;
    return *this;
}

ConfigurationBuilder& ConfigurationBuilder::tileSizeFromSlide(bool fromSlide) {
    writable(impl_).settings.tileSizeFromSlide = fromSlide;
    return *this;
}

ConfigurationBuilder& ConfigurationBuilder::logFilePath(const char* path) {
    Impl& impl = writable(impl_);
    impl.logFilePathNull = (path == nullptr);
    impl.settings.logFilePath = path == nullptr ? "" : path;
    return *this;
}

ConfigurationBuilder& ConfigurationBuilder::logLevel(LogLevel level) {
    writable(impl_).settings.logLevel = level;
    return *this;
}

ConfigurationBuilder& ConfigurationBuilder::eventPollIntervalMs(int32_t milliseconds) {
    writable(impl_).settings.eventPollIntervalMs = milliseconds;
    return *this;
}

ConfigurationBuilder& ConfigurationBuilder::eventSignalEnabled(bool enabled) {
    writable(impl_).settings.eventSignalEnabled = enabled;
    return *this;
}

ConfigurationBuilder& ConfigurationBuilder::objectBatchSize(int64_t size) {
    writable(impl_).settings.objectBatchSize = size;
    return *this;
}

ConfigurationBuilder& ConfigurationBuilder::scratchDirectory(const char* path) {
    Impl& impl = writable(impl_);
    impl.scratchDirectoryNull = (path == nullptr);
    impl.settings.scratchDirectory = path == nullptr ? "" : path;
    return *this;
}

Configuration ConfigurationBuilder::build() const {
    const Settings& s = settingsOf(impl_);
    std::vector<std::string> clauses;
    // Declaration order of the Configuration getters.
    if (s.memoryCeilingBytes <= 0)
        clauses.push_back("memoryCeilingBytes must be > 0 (got " + std::to_string(s.memoryCeilingBytes) + ")");
    if (s.workerCount < 1)
        clauses.push_back("workerCount must be >= 1 (got " + std::to_string(s.workerCount) + ")");
    if (s.tileWidth < 1)
        clauses.push_back("tileWidth must be >= 1 (got " + std::to_string(s.tileWidth) + ")");
    if (s.tileHeight < 1)
        clauses.push_back("tileHeight must be >= 1 (got " + std::to_string(s.tileHeight) + ")");
    if (impl_ && impl_->logFilePathNull)
        clauses.push_back("logFilePath must not be null");
    if (!logLevelInRange(s.logLevel))
        clauses.push_back("logLevel must be in Trace..Off (got " +
                          std::to_string(static_cast<int32_t>(s.logLevel)) + ")");
    if (s.eventPollIntervalMs < 1)
        clauses.push_back("eventPollIntervalMs must be >= 1 (got " + std::to_string(s.eventPollIntervalMs) + ")");
    if (s.objectBatchSize < 1)
        clauses.push_back("objectBatchSize must be >= 1 (got " + std::to_string(s.objectBatchSize) + ")");
    if (impl_ && impl_->scratchDirectoryNull)
        clauses.push_back("scratchDirectory must not be null");

    if (!clauses.empty()) {
        std::string message = "ConfigurationBuilder: invalid configuration: ";
        for (size_t i = 0; i < clauses.size(); ++i) {
            if (i != 0)
                message += "; ";
            message += clauses[i];
        }
        throw InvalidArgumentError(message.c_str());
    }

    Configuration built;
    built.impl_->settings = s;
    return built;
}

} // namespace slideio::processor
