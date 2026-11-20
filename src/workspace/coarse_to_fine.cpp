// Requirements: UR-SEG-8, UR-IMG-4, UR-STG-10
#include <slideio/processor/workspace/coarse_to_fine.hpp>

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/workspace/stage_definition.hpp>

#include <memory>
#include <string>
#include <utility>

namespace slideio::processor {

namespace {

void requireText(const char* text, const char* what) {
    if (text == nullptr || text[0] == '\0') {
        throw InvalidArgumentError((std::string("CoarseToFineSpec: ") + what + " is null or empty").c_str());
    }
}

[[noreturn]] void refuse(const std::string& what) {
    throw StageError(("declareCoarseToFine: " + what).c_str());
}

} // namespace

class CoarseToFineSpec::Impl {
public:
    std::string coarseName;
    std::string fineName;
    PipelineSpec coarsePipeline;
    PipelineSpec finePipeline;
    LayerId coarseLayer;
    LayerId fineLayer;
    bool hasCoarse = false;
    bool hasFine = false;
    std::string binding = "coarse";
    LayerRasterContent content = LayerRasterContent::ObjectAnchor;
    bool restrictFine = true;
};

namespace detail {

struct CoarseToFineAccess {
    static const CoarseToFineSpec::Impl& impl(const CoarseToFineSpec& spec) { return *spec.impl_; }
};

} // namespace detail

CoarseToFineSpec::CoarseToFineSpec(const char* coarseStageName, const char* fineStageName) : impl_(new Impl) {
    requireText(coarseStageName, "the coarse stage name");
    requireText(fineStageName, "the fine stage name");
    impl_->coarseName = coarseStageName;
    impl_->fineName = fineStageName;
}

CoarseToFineSpec::CoarseToFineSpec(const CoarseToFineSpec& other) : impl_(new Impl(*other.impl_)) {}

CoarseToFineSpec& CoarseToFineSpec::operator=(const CoarseToFineSpec& other) {
    if (this != &other) {
        std::unique_ptr<Impl> copy(new Impl(*other.impl_));
        impl_ = std::move(copy);
    }
    return *this;
}

CoarseToFineSpec::~CoarseToFineSpec() = default;

CoarseToFineSpec& CoarseToFineSpec::coarse(const PipelineSpec& pipeline, LayerId writes) {
    impl_->coarsePipeline = pipeline;
    impl_->coarseLayer = writes;
    impl_->hasCoarse = true;
    return *this;
}

CoarseToFineSpec& CoarseToFineSpec::fine(const PipelineSpec& pipeline, LayerId writes) {
    impl_->finePipeline = pipeline;
    impl_->fineLayer = writes;
    impl_->hasFine = true;
    return *this;
}

CoarseToFineSpec& CoarseToFineSpec::binding(const char* name, LayerRasterContent content) {
    requireText(name, "the binding name");
    impl_->binding = name;
    impl_->content = content;
    return *this;
}

CoarseToFineSpec& CoarseToFineSpec::restrictFineToCoarse(bool restrict) noexcept {
    impl_->restrictFine = restrict;
    return *this;
}

void declareCoarseToFine(StageRegistry& registry, const CoarseToFineSpec& spec, StageId& coarseOut,
                         StageId& fineOut) {
    const auto& s = detail::CoarseToFineAccess::impl(spec);

    // Everything is checked before anything is declared (SDS 04 4.2).
    if (!s.hasCoarse) refuse("the coarse half was never set (\"coarse\")");
    if (!s.hasFine) refuse("the fine half was never set (\"fine\")");
    if (s.coarseName == s.fineName) {
        refuse("both stages are named \"" + s.coarseName + "\" (\"names\")");
    }
    if (s.coarseLayer == s.fineLayer) {
        refuse("stages \"" + s.coarseName + "\" and \"" + s.fineName + "\" write the same layer (\"layers\")");
    }
    const int32_t coarseLevel = s.coarsePipeline.analysisLevel();
    const int32_t fineLevel = s.finePipeline.analysisLevel();
    if (coarseLevel >= 0 && fineLevel >= 0 && coarseLevel <= fineLevel) {
        refuse("stage \"" + s.coarseName + "\" must run at a strictly coarser level than stage \"" + s.fineName +
               "\" (\"levels\")");
    }

    StageDefinition coarse(s.coarseName.c_str());
    coarse.pipeline(s.coarsePipeline).writesLayer(s.coarseLayer);

    StageDefinition fine(s.fineName.c_str());
    fine.pipeline(s.finePipeline)
        .writesLayer(s.fineLayer)
        .readsLayerAsRaster(s.binding.c_str(), s.coarseLayer, s.content);
    if (s.restrictFine) fine.restrictToLayer(s.coarseLayer);

    // There is no transaction across two declarations: when the second throws, the
    // first stays declared and the error propagates unchanged.
    const StageId coarseId = registry.declare(coarse);
    const StageId fineId = registry.declare(fine);
    coarseOut = coarseId;
    fineOut = fineId;
}

} // namespace slideio::processor
