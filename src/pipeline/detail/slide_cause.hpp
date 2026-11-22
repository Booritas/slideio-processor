// Requirements: UR-EXE-5
#pragma once

// The text of an exception raised while SlideIO described or read a scene, as an error
// cause: SlideIO may word its errors with the slide path, which can carry a patient
// identifier, so the path is replaced by "<slide>" (core/detail/diagnostics.hpp).

#include "core/detail/diagnostics.hpp"

#include <slideio/slideio/scene.hpp>

#include <exception>
#include <string>

namespace slideio::processor::detail {

// diagnosticText(e) with the path of `scene`'s file replaced by "<slide>". When the scene
// cannot name its file, the text is `diagnosticText(e)` with nothing replaced.
inline std::string slideCauseText(const std::exception& e, const slideio::Scene& scene) {
    std::string text = diagnosticText(e);
    try {
        text = redactSlidePath(text, scene.getFilePath());
    } catch (...) {
    }
    return text;
}

} // namespace slideio::processor::detail
