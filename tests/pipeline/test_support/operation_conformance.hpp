// Requirements: UR-EXT-1, UR-EXT-5, UR-ALG-4, UR-STG-4
#pragma once

// The conformance test SDS 05 5.1 calls for, reusable by any library that
// registers operations (`algorithm` runs it for every built-in).

namespace slideio::processor::testing_support {

/// For the operation registered under @p name: creates it from default
/// OperationParameters, expects the parameter-independent declarations (name,
/// arity, supportedTypes, producesObjects) to equal the catalog's, then
/// round-trips create(name(), parameters()) and expects the same declarations
/// (halo and the other parameter-dependent ones included) and equal parameters.
/// Reports through gtest expectations.
void expectConformant(const char* name);

} // namespace slideio::processor::testing_support
