#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/error.hpp>

#include <mutex>
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

    static int32_t defaultWorkerCount() {
        const unsigned hardware = std::thread::hardware_concurrency();
        return hardware == 0 ? 1 : static_cast<int32_t>(hardware);
    }
};

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
// any initialisation-order dependence.
struct ProcessDefaultRegistry {
    std::mutex mutex;
    std::vector<std::unique_ptr<Configuration>> published;
    const Configuration* current = nullptr;
};

ProcessDefaultRegistry& registry() {
    static ProcessDefaultRegistry instance;
    return instance;
}

} // namespace

class Configuration::Impl {
public:
    Settings settings;
};

Configuration::Configuration() : impl_(std::make_unique<Impl>()) {}

Configuration::Configuration(const Configuration& other) : impl_(std::make_unique<Impl>(*other.impl_)) {}

Configuration::Configuration(Configuration&& other) noexcept : impl_(std::move(other.impl_)) {
    other.impl_ = std::make_unique<Impl>();
}

Configuration& Configuration::operator=(const Configuration& other) {
    if (this != &other)
        impl_ = std::make_unique<Impl>(*other.impl_);
    return *this;
}

Configuration& Configuration::operator=(Configuration&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_);
        other.impl_ = std::make_unique<Impl>();
    }
    return *this;
}

Configuration::~Configuration() = default;

Configuration Configuration::defaults() noexcept {
    return Configuration();
}

const Configuration& Configuration::processDefault() noexcept {
    ProcessDefaultRegistry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    if (r.current == nullptr) {
        r.published.push_back(std::make_unique<Configuration>());
        r.current = r.published.back().get();
    }
    return *r.current;
}

void Configuration::setProcessDefault(const Configuration& configuration) {
    auto next = std::make_unique<Configuration>(configuration);
    ProcessDefaultRegistry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    r.published.push_back(std::move(next));
    r.current = r.published.back().get();
}

int64_t Configuration::memoryCeilingBytes() const noexcept {
    return impl_->settings.memoryCeilingBytes;
}
int32_t Configuration::workerCount() const noexcept {
    return impl_->settings.workerCount;
}
int32_t Configuration::tileWidth() const noexcept {
    return impl_->settings.tileWidth;
}
int32_t Configuration::tileHeight() const noexcept {
    return impl_->settings.tileHeight;
}
bool Configuration::tileSizeFromSlide() const noexcept {
    return impl_->settings.tileSizeFromSlide;
}
const char* Configuration::logFilePath() const noexcept {
    return impl_->settings.logFilePath.c_str();
}
LogLevel Configuration::logLevel() const noexcept {
    return impl_->settings.logLevel;
}
int32_t Configuration::eventPollIntervalMs() const noexcept {
    return impl_->settings.eventPollIntervalMs;
}
bool Configuration::eventSignalEnabled() const noexcept {
    return impl_->settings.eventSignalEnabled;
}
int64_t Configuration::objectBatchSize() const noexcept {
    return impl_->settings.objectBatchSize;
}
const char* Configuration::scratchDirectory() const noexcept {
    return impl_->settings.scratchDirectory.c_str();
}

bool Configuration::equals(const Configuration& other) const noexcept {
    return equal(impl_->settings, other.impl_->settings);
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
    impl_->settings = from.impl_->settings;
}

ConfigurationBuilder::ConfigurationBuilder(const ConfigurationBuilder& other)
    : impl_(std::make_unique<Impl>(*other.impl_)) {}

ConfigurationBuilder::ConfigurationBuilder(ConfigurationBuilder&& other) noexcept : impl_(std::move(other.impl_)) {
    other.impl_ = std::make_unique<Impl>();
}

ConfigurationBuilder& ConfigurationBuilder::operator=(const ConfigurationBuilder& other) {
    if (this != &other)
        impl_ = std::make_unique<Impl>(*other.impl_);
    return *this;
}

ConfigurationBuilder& ConfigurationBuilder::operator=(ConfigurationBuilder&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_);
        other.impl_ = std::make_unique<Impl>();
    }
    return *this;
}

ConfigurationBuilder::~ConfigurationBuilder() = default;

ConfigurationBuilder& ConfigurationBuilder::memoryCeilingBytes(int64_t bytes) {
    impl_->settings.memoryCeilingBytes = bytes;
    return *this;
}

ConfigurationBuilder& ConfigurationBuilder::workerCount(int32_t count) {
    impl_->settings.workerCount = count;
    return *this;
}

ConfigurationBuilder& ConfigurationBuilder::tileSize(int32_t width, int32_t height) {
    impl_->settings.tileWidth = width;
    impl_->settings.tileHeight = height;
    return *this;
}

ConfigurationBuilder& ConfigurationBuilder::tileSizeFromSlide(bool fromSlide) {
    impl_->settings.tileSizeFromSlide = fromSlide;
    return *this;
}

ConfigurationBuilder& ConfigurationBuilder::logFilePath(const char* path) {
    impl_->logFilePathNull = (path == nullptr);
    impl_->settings.logFilePath = path == nullptr ? "" : path;
    return *this;
}

ConfigurationBuilder& ConfigurationBuilder::logLevel(LogLevel level) {
    impl_->settings.logLevel = level;
    return *this;
}

ConfigurationBuilder& ConfigurationBuilder::eventPollIntervalMs(int32_t milliseconds) {
    impl_->settings.eventPollIntervalMs = milliseconds;
    return *this;
}

ConfigurationBuilder& ConfigurationBuilder::eventSignalEnabled(bool enabled) {
    impl_->settings.eventSignalEnabled = enabled;
    return *this;
}

ConfigurationBuilder& ConfigurationBuilder::objectBatchSize(int64_t size) {
    impl_->settings.objectBatchSize = size;
    return *this;
}

ConfigurationBuilder& ConfigurationBuilder::scratchDirectory(const char* path) {
    impl_->scratchDirectoryNull = (path == nullptr);
    impl_->settings.scratchDirectory = path == nullptr ? "" : path;
    return *this;
}

Configuration ConfigurationBuilder::build() const {
    const Settings& s = impl_->settings;
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
    if (impl_->logFilePathNull)
        clauses.push_back("logFilePath must not be null");
    if (!logLevelInRange(s.logLevel))
        clauses.push_back("logLevel must be in Trace..Off (got " +
                          std::to_string(static_cast<int32_t>(s.logLevel)) + ")");
    if (s.eventPollIntervalMs < 1)
        clauses.push_back("eventPollIntervalMs must be >= 1 (got " + std::to_string(s.eventPollIntervalMs) + ")");
    if (s.objectBatchSize < 1)
        clauses.push_back("objectBatchSize must be >= 1 (got " + std::to_string(s.objectBatchSize) + ")");
    if (impl_->scratchDirectoryNull)
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
