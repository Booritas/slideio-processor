// Requirements: UR-SEG-3, UR-QA-2, UR-QA-3
#include <slideio/processor/pipeline/seam_merger.hpp>

#include <pipeline/detail/combine_partials.hpp>
#include <pipeline/detail/fragment_merge.hpp>
#include <pipeline/detail/seam_merger_access.hpp>

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/pipeline/standard_measurements.hpp>

#include <algorithm>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>
#include <tuple>

namespace slideio::processor {

namespace {

std::string layerText(LayerId layer) { return "layer " + std::to_string(layer.value()); }

void checkTile(const TileGrid& grid, int64_t tileIndex, const char* where) {
    if (tileIndex < 0 || tileIndex >= grid.tileCount()) {
        throw InvalidArgumentError((std::string("SeamMerger::") + where + ": tile index " +
                                    std::to_string(tileIndex) + " is outside the grid of " +
                                    std::to_string(grid.tileCount()) + " tiles")
                                       .c_str());
    }
}

} // namespace

SeamMerger::Impl::Impl(const TileGrid& grid_, MemoryBudgetPtr budget_, std::string scratchDirectory_)
    : grid(grid_), budget(std::move(budget_)), scratchDirectory(std::move(scratchDirectory_)),
      completed_(static_cast<size_t>(grid_.tileCount()), 0) {}

int64_t SeamMerger::Impl::neighbourAcross(int64_t tileIndex, const detail::SeamContact& contact) const {
    const int64_t column = tileIndex % grid.columns();
    const int64_t row = tileIndex / grid.columns();
    const Rect64 rect = grid.tileRect(tileIndex);
    if (contact.vertical) {
        return grid.tileIndex(contact.line == rect.x ? column - 1 : column + 1, row);
    }
    return grid.tileIndex(column, contact.line == rect.y ? row - 1 : row + 1);
}

int64_t SeamMerger::Impl::edgeKey(int64_t a, int64_t b) const {
    return std::min(a, b) * grid.tileCount() + std::max(a, b);
}

int64_t SeamMerger::Impl::find(int64_t fragment) {
    int64_t root = fragment;
    while (parent_[static_cast<size_t>(root)] != root) {
        root = parent_[static_cast<size_t>(root)];
    }
    while (parent_[static_cast<size_t>(fragment)] != root) {
        const int64_t next = parent_[static_cast<size_t>(fragment)];
        parent_[static_cast<size_t>(fragment)] = root;
        fragment = next;
    }
    return root;
}

void SeamMerger::Impl::unite(int64_t a, int64_t b) {
    int64_t rootA = find(a);
    int64_t rootB = find(b);
    if (rootA == rootB) {
        return;
    }
    detail::SeamComponent* big = &components_.at(rootA);
    detail::SeamComponent* small = &components_.at(rootB);
    if (big->members.size() < small->members.size()) {
        std::swap(big, small);
        std::swap(rootA, rootB);
    }
    big->members.insert(big->members.end(), small->members.begin(), small->members.end());
    big->openTiles.insert(small->openTiles.begin(), small->openTiles.end());
    parent_[static_cast<size_t>(rootB)] = rootA;
    const int64_t size = static_cast<int64_t>(big->members.size());
    components_.erase(rootB);
    if (size > largestComponentFragmentCount.load()) {
        largestComponentFragmentCount.store(size);
    }
}

void SeamMerger::Impl::add(std::unique_ptr<detail::SeamFragment> fragment, LayerId layer,
                           const std::vector<detail::SeamContact>& contacts,
                           const std::vector<int64_t>& neighbours) {
    std::lock_guard<std::mutex> lock(mutex);
    const int64_t id = static_cast<int64_t>(fragments_.size());
    const int64_t tile = fragment->tile;

    detail::SeamComponent component;
    component.layer = layer;
    component.members.push_back(id);
    if (completed_[static_cast<size_t>(tile)] == 0) {
        component.openTiles.insert(tile);
    }
    for (int64_t neighbour : neighbours) {
        if (completed_[static_cast<size_t>(neighbour)] == 0) {
            component.openTiles.insert(neighbour);
        }
    }
    components_.emplace(id, std::move(component));
    fragments_.push_back(std::move(fragment));
    parent_.push_back(id);
    touching_[tile].push_back(id);
    for (int64_t neighbour : neighbours) {
        touching_[neighbour].push_back(id);
    }

    // Join with the fragments across each contact's seam, then wait there for
    // the ones still to come.
    for (size_t i = 0; i < contacts.size(); ++i) {
        std::vector<detail::SeamEdgeContact>& waiting = edges_[edgeKey(tile, neighbourAcross(tile, contacts[i]))];
        for (const detail::SeamEdgeContact& other : waiting) {
            if (other.tile != tile && fragments_[static_cast<size_t>(other.fragment)] != nullptr &&
                components_.at(find(other.fragment)).layer == layer &&
                detail::contactsJoin(other.contact, contacts[i])) {
                unite(id, other.fragment);
            }
        }
        waiting.push_back(detail::SeamEdgeContact{id, tile, contacts[i]});
    }

    ++fragmentCount;
    ++residentFragmentCount;
    if (largestComponentFragmentCount.load() < 1) {
        largestComponentFragmentCount.store(1);
    }
}

detail::ClosedComponent SeamMerger::Impl::takeComponent(int64_t root) {
    auto it = components_.find(root);
    detail::ClosedComponent closed;
    closed.layer = it->second.layer;
    for (int64_t member : it->second.members) {
        closed.fragments.push_back(std::move(fragments_[static_cast<size_t>(member)]));
    }
    residentFragmentCount -= static_cast<int64_t>(closed.fragments.size());
    components_.erase(it);
    return closed;
}

std::vector<detail::ClosedComponent> SeamMerger::Impl::complete(int64_t tileIndex) {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<detail::ClosedComponent> closed;
    if (completed_[static_cast<size_t>(tileIndex)] != 0) {
        return closed;
    }
    completed_[static_cast<size_t>(tileIndex)] = 1;

    // No fragment arrives any more on an edge whose two tiles have completed.
    const int64_t column = tileIndex % grid.columns();
    const int64_t row = tileIndex / grid.columns();
    const int64_t around[4][2] = {{column - 1, row}, {column + 1, row}, {column, row - 1}, {column, row + 1}};
    for (const auto& at : around) {
        if (at[0] < 0 || at[0] >= grid.columns() || at[1] < 0 || at[1] >= grid.rows()) {
            continue;
        }
        const int64_t neighbour = grid.tileIndex(at[0], at[1]);
        if (completed_[static_cast<size_t>(neighbour)] != 0) {
            edges_.erase(edgeKey(tileIndex, neighbour));
        }
    }

    const auto touching = touching_.find(tileIndex);
    if (touching == touching_.end()) {
        return closed;
    }
    const std::vector<int64_t> fragments = std::move(touching->second);
    touching_.erase(touching);
    std::vector<int64_t> roots;
    for (int64_t fragment : fragments) {
        if (fragments_[static_cast<size_t>(fragment)] != nullptr) {
            roots.push_back(find(fragment));
        }
    }
    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    for (int64_t root : roots) {
        detail::SeamComponent& component = components_.at(root);
        component.openTiles.erase(tileIndex);
        if (component.openTiles.empty()) {
            closed.push_back(takeComponent(root));
        }
    }
    return closed;
}

std::vector<detail::ClosedComponent> SeamMerger::Impl::takeRemaining() {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& entry : components_) {
        if (!entry.second.openTiles.empty()) {
            throw InternalError(("SeamMerger::resolve: an object in " + layerText(entry.second.layer) +
                                 " is still open: " + std::to_string(entry.second.openTiles.size()) +
                                 " of its tiles never completed (" + std::to_string(entry.second.members.size()) +
                                 " fragments)")
                                    .c_str());
        }
    }
    std::vector<int64_t> roots;
    for (const auto& entry : components_) {
        roots.push_back(entry.first);
    }
    std::vector<detail::ClosedComponent> closed;
    for (int64_t root : roots) {
        closed.push_back(takeComponent(root));
    }
    touching_.clear();
    edges_.clear();
    return closed;
}

std::function<bool(Point64)> SeamMerger::Impl::currentFilter() const {
    std::lock_guard<std::mutex> lock(mutex);
    return filter;
}

void SeamMerger::Impl::emit(detail::ClosedComponent& component, ObjectSink& sink) {
    // Canonical fragment order: ascending anchor (y, then x), ties by tile. The
    // float sums below are combined in this order, never in arrival order.
    std::vector<const detail::SeamFragment*> ordered;
    for (const auto& fragment : component.fragments) {
        ordered.push_back(fragment.get());
    }
    std::sort(ordered.begin(), ordered.end(), [](const detail::SeamFragment* a, const detail::SeamFragment* b) {
        return std::make_tuple(a->anchor.y, a->anchor.x, a->tile) < std::make_tuple(b->anchor.y, b->anchor.x, b->tile);
    });

    std::vector<const Polygon*> polygons;
    for (const detail::SeamFragment* fragment : ordered) {
        polygons.push_back(&fragment->polygon);
    }
    Polygon merged;
    try {
        merged = detail::mergeFragments(polygons);
    } catch (const InternalError& e) {
        throw InternalError((layerText(component.layer) + ": " + e.what()).c_str());
    }

    std::map<int32_t, IntensityPartial> combined;
    for (const detail::SeamFragment* fragment : ordered) {
        for (const IntensityPartial& partial : fragment->partials) {
            const auto found = combined.find(partial.channel);
            if (found == combined.end()) {
                combined.emplace(partial.channel, partial);
            } else {
                found->second = detail::combinePartials(found->second, partial);
            }
        }
    }
    std::vector<IntensityPartial> partials;
    for (const auto& entry : combined) {
        partials.push_back(entry.second);
    }

    ObjectRecord record;
    record.setGeometry(merged);
    record.setLayer(component.layer);
    applyStandardMeasurements(merged, partials.data(), static_cast<int32_t>(partials.size()),
                              record.measurements());
    component.fragments.clear();
    ++mergedObjectCount;

    const std::function<bool(Point64)> admits = currentFilter();
    if (admits && !admits(merged.anchor())) {
        return;
    }
    sink.emit(record);
}

SeamMerger::SeamMerger(const TileGrid& grid, MemoryBudgetPtr budget, const char* scratchDirectory) {
    if (!budget) {
        throw InvalidArgumentError("SeamMerger: the memory budget must not be null");
    }
    if (scratchDirectory == nullptr) {
        throw InvalidArgumentError("SeamMerger: the scratch directory must not be null");
    }
    std::error_code error;
    if (!std::filesystem::is_directory(std::filesystem::u8path(scratchDirectory), error)) {
        throw InvalidArgumentError(
            (std::string("SeamMerger: the scratch directory '") + scratchDirectory + "' is not a directory").c_str());
    }
    impl_ = std::make_unique<Impl>(grid, std::move(budget), std::string(scratchDirectory));
}

SeamMerger::~SeamMerger() = default;

void SeamMerger::addFragment(int64_t tileIndex, const Polygon& fragment, LayerId layer,
                             const IntensityPartial* partials, int32_t partialCount) {
    checkTile(impl_->grid, tileIndex, "addFragment");
    if (fragment.isEmpty()) {
        throw InvalidArgumentError("SeamMerger::addFragment: the fragment is empty");
    }
    if (!layer.isValid()) {
        throw InvalidArgumentError("SeamMerger::addFragment: the layer is invalid");
    }
    if (partialCount < 0) {
        throw InvalidArgumentError("SeamMerger::addFragment: the partial count is negative");
    }
    if (partials == nullptr && partialCount > 0) {
        throw InvalidArgumentError("SeamMerger::addFragment: partials are null but the partial count is positive");
    }
    auto held = std::make_unique<detail::SeamFragment>();
    held->tile = tileIndex;
    held->polygon = fragment;
    held->anchor = fragment.anchor();
    held->partials.assign(partials, partials + partialCount);
    for (size_t i = 0; i < held->partials.size(); ++i) {
        for (size_t j = 0; j < i; ++j) {
            if (held->partials[i].channel == held->partials[j].channel) {
                throw InvalidArgumentError(
                    ("SeamMerger::addFragment: two partials name channel " + std::to_string(held->partials[i].channel))
                        .c_str());
            }
        }
    }

    const std::vector<detail::SeamContact> contacts = detail::seamContacts(fragment, impl_->grid, tileIndex);
    std::vector<int64_t> neighbours;
    for (const detail::SeamContact& contact : contacts) {
        neighbours.push_back(impl_->neighbourAcross(tileIndex, contact));
    }
    std::sort(neighbours.begin(), neighbours.end());
    neighbours.erase(std::unique(neighbours.begin(), neighbours.end()), neighbours.end());

    impl_->add(std::move(held), layer, contacts, neighbours);
}

void SeamMerger::notifyTileComplete(int64_t tileIndex, ObjectSink& sink) {
    checkTile(impl_->grid, tileIndex, "notifyTileComplete");
    std::vector<detail::ClosedComponent> closed = impl_->complete(tileIndex);
    for (detail::ClosedComponent& component : closed) {
        impl_->emit(component, sink);
    }
}

void SeamMerger::resolve(ObjectSink& sink, ProgressObserver* progress, const CancellationToken& token) {
    token.throwIfCancelled();
    std::vector<detail::ClosedComponent> closed = impl_->takeRemaining();
    const int64_t total = static_cast<int64_t>(closed.size());
    for (int64_t done = 0; done < total; ++done) {
        token.throwIfCancelled();
        impl_->emit(closed[static_cast<size_t>(done)], sink);
        if (progress != nullptr) {
            progress->onProgress("seam merge", static_cast<double>(done + 1) / static_cast<double>(total), done + 1,
                                 total);
        }
    }
}

int64_t SeamMerger::fragmentCount() const noexcept { return impl_->fragmentCount.load(); }

int64_t SeamMerger::mergedObjectCount() const noexcept { return impl_->mergedObjectCount.load(); }

int64_t SeamMerger::residentFragmentCount() const noexcept { return impl_->residentFragmentCount.load(); }

int64_t SeamMerger::spilledFragmentCount() const noexcept { return 0; }

int64_t SeamMerger::spilledBytes() const noexcept { return 0; }

int64_t SeamMerger::largestComponentFragmentCount() const noexcept {
    return impl_->largestComponentFragmentCount.load();
}

} // namespace slideio::processor
