// Requirements: UR-STG-4, UR-RES-11
#include <pipeline/detail/parameters_json.hpp>

#include <slideio/processor/core/error.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace slideio::processor::detail {

namespace {

[[noreturn]] void fail(const char* where, const std::string& key, const char* what) {
    throw InvalidArgumentError((std::string(where != nullptr ? where : "parameters") + ": parameter '" + key +
                                "' " + what).c_str());
}

bool hasEmbeddedNul(const rapidjson::Value& s) {
    return std::strlen(s.GetString()) != static_cast<size_t>(s.GetStringLength());
}

} // namespace

void parametersToJson(const OperationParameters& parameters, rapidjson::Writer<rapidjson::StringBuffer>& writer) {
    writer.StartObject();
    for (int32_t i = 0; i < parameters.count(); ++i) {
        const char* key = parameters.keyAt(i);
        writer.Key(key, static_cast<rapidjson::SizeType>(std::strlen(key)));
        int64_t iv = 0;
        double dv = 0;
        bool bv = false;
        if (parameters.getInt(key, iv)) {
            writer.Int64(iv);
        } else if (parameters.getDouble(key, dv)) {
            writer.Double(dv);
        } else if (parameters.getBool(key, bv)) {
            writer.Bool(bv);
        } else if (const char* sv = parameters.getString(key, nullptr)) {
            writer.String(sv, static_cast<rapidjson::SizeType>(std::strlen(sv)));
        } else {
            std::vector<double> values(static_cast<size_t>(parameters.getDoubleArray(key, nullptr, 0)));
            if (!values.empty()) {
                parameters.getDoubleArray(key, values.data(), static_cast<int32_t>(values.size()));
            }
            writer.StartArray();
            for (double v : values) {
                writer.Double(v);
            }
            writer.EndArray();
        }
    }
    writer.EndObject();
}

OperationParameters parametersFromJson(const rapidjson::Value& value, const char* where) {
    if (!value.IsObject()) {
        throw InvalidArgumentError((std::string(where != nullptr ? where : "parameters") +
                                    ": the parameters must be a JSON object").c_str());
    }
    OperationParameters result;
    for (auto it = value.MemberBegin(); it != value.MemberEnd(); ++it) {
        const std::string key(it->name.GetString(), it->name.GetStringLength());
        if (key.empty() || hasEmbeddedNul(it->name)) {
            fail(where, key, "has an empty key or a key with an embedded NUL");
        }
        const rapidjson::Value& v = it->value;
        if (v.IsString()) {
            if (hasEmbeddedNul(v)) {
                fail(where, key, "holds a string with an embedded NUL");
            }
            result.setString(key.c_str(), v.GetString());
        } else if (v.IsBool()) {
            result.setBool(key.c_str(), v.GetBool());
        } else if (v.IsInt64()) {
            result.setInt(key.c_str(), v.GetInt64());
        } else if (v.IsUint64()) {
            fail(where, key, "holds an integer outside the 64-bit signed range");
        } else if (v.IsNumber()) {
            if (!std::isfinite(v.GetDouble())) {
                fail(where, key, "holds a non-finite number");
            }
            result.setDouble(key.c_str(), v.GetDouble());
        } else if (v.IsArray()) {
            std::vector<double> values;
            values.reserve(v.Size());
            for (const rapidjson::Value& e : v.GetArray()) {
                if (!e.IsNumber()) {
                    fail(where, key, "holds an array with a non-numeric element");
                }
                values.push_back(e.GetDouble());
            }
            result.setDoubleArray(key.c_str(), values.data(), static_cast<int32_t>(values.size()));
        } else {
            fail(where, key, "holds a value that is not a number, bool, string or array of numbers");
        }
    }
    return result;
}

bool parametersEqual(const OperationParameters& a, const OperationParameters& b) {
    if (a.count() != b.count()) {
        return false;
    }
    for (int32_t i = 0; i < a.count(); ++i) {
        const char* key = a.keyAt(i);
        if (std::strcmp(key, b.keyAt(i)) != 0) {
            return false;
        }
        int64_t ai = 0, bi = 0;
        double ad = 0, bd = 0;
        bool ab = false, bb = false;
        if (a.getInt(key, ai)) {
            if (!b.getInt(key, bi) || ai != bi) {
                return false;
            }
        } else if (a.getDouble(key, ad)) {
            if (!b.getDouble(key, bd) || ad != bd) {
                return false;
            }
        } else if (a.getBool(key, ab)) {
            if (!b.getBool(key, bb) || ab != bb) {
                return false;
            }
        } else if (const char* as = a.getString(key, nullptr)) {
            const char* bs = b.getString(key, nullptr);
            if (bs == nullptr || std::strcmp(as, bs) != 0) {
                return false;
            }
        } else {
            const int32_t n = a.getDoubleArray(key, nullptr, 0);
            if (b.getInt(key, bi) || b.getDouble(key, bd) || b.getBool(key, bb) ||
                b.getString(key, nullptr) != nullptr || n != b.getDoubleArray(key, nullptr, 0)) {
                return false;
            }
            std::vector<double> av(static_cast<size_t>(n)), bv(static_cast<size_t>(n));
            if (n > 0) {
                a.getDoubleArray(key, av.data(), n);
                b.getDoubleArray(key, bv.data(), n);
            }
            if (av != bv) {
                return false;
            }
        }
    }
    return true;
}

} // namespace slideio::processor::detail
