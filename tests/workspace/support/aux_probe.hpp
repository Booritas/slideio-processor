// Requirements: UR-STG-10
// Test-only: "test.ws.auxProbe", an operation that reads the auxiliary raster bound under
// the name given by its string parameter "binding" and records what it saw, and
// "test.ws.auxAdd", which adds it to its input.
#pragma once

#include <cstdint>
#include <string>

namespace slideio::processor::testing_support {

// Registers "test.ws.auxProbe", "test.ws.auxAdd" (each plane of the output is the input
// plane plus the same Float32 plane of the auxiliary raster bound under "binding", or the
// input alone) and the other test operations, skipping any already registered. The
// probe is PerChannel, Byte, halo 0, and leaves the output as allocated (zero-initialised):
// it only observes.
void registerAuxProbe();

// Clears what the probe has recorded.
void resetAuxProbe();

// The number of tiles on which the probe saw `binding` bound.
int64_t auxProbeTiles(const std::string& binding);

} // namespace slideio::processor::testing_support
