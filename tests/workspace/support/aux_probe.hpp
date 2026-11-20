// Requirements: UR-STG-10
// Test-only: "test.ws.auxProbe", an operation that reads the auxiliary raster bound under
// the name given by its string parameter "binding" and records what it saw.
#pragma once

#include <cstdint>
#include <string>

namespace slideio::processor::testing_support {

// Registers "test.ws.auxProbe" (and the other test operations), skipping any already
// registered. PerChannel, Byte, halo 0, output Byte zeros: the probe only observes.
void registerAuxProbe();

// Clears what the probe has recorded.
void resetAuxProbe();

// The number of tiles on which the probe saw `binding` bound.
int64_t auxProbeTiles(const std::string& binding);

} // namespace slideio::processor::testing_support
