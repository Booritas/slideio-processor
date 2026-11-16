// Requirements: UR-STG-4, UR-RES-11
#pragma once

#include <slideio/processor/pipeline/operation_parameters.hpp>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace slideio::processor::detail {

/// Writes the bag as one JSON object, keys in stored order (ruling P7).
void parametersToJson(const OperationParameters& parameters,
                      rapidjson::Writer<rapidjson::StringBuffer>& writer);

/// Reads a bag from a JSON object: an integer reads as int, a number with a
/// fraction or exponent as double, an array as a double array. Throws
/// InvalidArgumentError naming @p where and the key.
OperationParameters parametersFromJson(const rapidjson::Value& value, const char* where);

/// Same keys, same types, same values (== on doubles, element-wise on arrays).
bool parametersEqual(const OperationParameters& a, const OperationParameters& b);

} // namespace slideio::processor::detail
