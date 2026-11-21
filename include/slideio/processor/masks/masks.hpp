// Requirements: UR-RES-7
#pragma once

/**
 * @file masks.hpp
 * @brief Umbrella header: includes every public header of the masks module.
 *
 * The masks module is part of the engine library: it presents label masks as SlideIO
 * scenes (label_scene.hpp) and writes label masks and processing maps, given as SlideIO
 * scenes, as lossless OME-TIFF (mask_writer.hpp). It links SlideIO, which the data library
 * never does.
 *
 * Thread-safety: not applicable (declarations only).
 * Requirements: UR-RES-7.
 */

#include <slideio/processor/masks/label_scene.hpp>
#include <slideio/processor/masks/mask_writer.hpp>
