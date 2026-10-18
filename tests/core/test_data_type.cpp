// Requirements: UR-IMG-8, UR-IMG-11, UR-IMG-12
#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/error.hpp>

#include <slideio/core/slideio_enums.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <utility>

using namespace slideio::processor;

TEST(DataType, SlideioMappingMatchesInstalledHeader) {
    using S = slideio::DataType;
    const std::pair<S, DataType> m[] = {{S::DT_Byte,DataType::Byte},{S::DT_Int8,DataType::Int8},
      {S::DT_UInt16,DataType::UInt16},{S::DT_Int16,DataType::Int16},{S::DT_UInt32,DataType::UInt32},
      {S::DT_Int32,DataType::Int32},{S::DT_UInt64,DataType::UInt64},{S::DT_Int64,DataType::Int64},
      {S::DT_Float16,DataType::Float16},{S::DT_Float32,DataType::Float32},{S::DT_Float64,DataType::Float64}};
    for (auto [s, p] : m) {
        EXPECT_EQ(dataTypeFromSlideio(static_cast<int32_t>(s)), p);
        EXPECT_EQ(dataTypeToSlideio(p), static_cast<int32_t>(s));
    }
    EXPECT_EQ(dataTypeFromSlideio(static_cast<int32_t>(S::DT_None)), DataType::Unknown);
    EXPECT_EQ(dataTypeFromSlideio(-5), DataType::Unknown);
    EXPECT_EQ(dataTypeToSlideio(DataType::Unknown), static_cast<int32_t>(S::DT_Unknown));
    EXPECT_EQ(dataTypeToSlideio(DataType::Unknown), 1024);
}

TEST(DataType, SizesNamesAndPredicates) {
    EXPECT_EQ(dataTypeSizeBytes(DataType::Float16), 2); EXPECT_EQ(dataTypeSizeBytes(DataType::UInt64), 8);
    EXPECT_EQ(dataTypeSizeBytes(DataType::Unknown), 0); EXPECT_STREQ(dataTypeName(DataType::Int16), "Int16");
    EXPECT_STREQ(dataTypeName(static_cast<DataType>(99)), "Unknown");
    EXPECT_TRUE(dataTypeIsFloat(DataType::Float16)); EXPECT_FALSE(dataTypeIsFloat(DataType::Int64));
    EXPECT_TRUE(dataTypeIsSigned(DataType::Int8));   EXPECT_FALSE(dataTypeIsSigned(DataType::UInt32));
    EXPECT_TRUE(dataTypeIsSigned(DataType::Float32));
}

TEST(DataType, EnumeratorOrderAndSizesTable) {
    const int32_t sizes[] = {1, 1, 2, 2, 4, 4, 8, 8, 2, 4, 8};
    for (int32_t i = 0; i < 11; ++i) {
        EXPECT_EQ(dataTypeSizeBytes(static_cast<DataType>(i)), sizes[i]) << i;
    }
    EXPECT_EQ(static_cast<int32_t>(DataType::Unknown), 11);
    EXPECT_STREQ(dataTypeName(DataType::UInt16), "UInt16");
    EXPECT_STREQ(dataTypeName(DataType::Unknown), "Unknown");
}

TEST(DataTypeSet, Algebra) {
    EXPECT_EQ(DataTypeSet::all().count(), 11); EXPECT_EQ(DataTypeSet::integerTypes().count(), 8);
    EXPECT_EQ(DataTypeSet::floatTypes().count(), 3); EXPECT_TRUE(DataTypeSet().isEmpty());
    DataTypeSet s; s.add(DataType::Float32).add(DataType::Byte).add(DataType::Unknown);
    EXPECT_EQ(s.count(), 2); EXPECT_EQ(s.at(0), DataType::Byte); EXPECT_EQ(s.at(1), DataType::Float32);
    EXPECT_THROW(s.at(2), InvalidArgumentError);
    EXPECT_EQ(s.intersect(DataTypeSet::floatTypes()), DataTypeSet().add(DataType::Float32));
    EXPECT_EQ(s.remove(DataType::Byte).count(), 1);
    EXPECT_EQ(DataTypeSet().add(DataType::Int8).bits(), 1u << 1);
}

TEST(DataTypeSet, ContainsAndBoundaries) {
    EXPECT_TRUE(DataTypeSet::all().contains(DataType::Float64));
    EXPECT_FALSE(DataTypeSet::all().contains(DataType::Unknown));
    EXPECT_FALSE(DataTypeSet::integerTypes().contains(DataType::Float16));
    EXPECT_FALSE(DataTypeSet::floatTypes().contains(DataType::Byte));
    EXPECT_FALSE(DataTypeSet::all().contains(static_cast<DataType>(99)));
    DataTypeSet s;
    s.add(static_cast<DataType>(99)).add(static_cast<DataType>(-1));
    EXPECT_TRUE(s.isEmpty());
    EXPECT_THROW(s.at(0), InvalidArgumentError);
    EXPECT_THROW(DataTypeSet::all().at(-1), InvalidArgumentError);
    EXPECT_EQ(DataTypeSet::all().at(10), DataType::Float64);
    EXPECT_FALSE(DataTypeSet() == DataTypeSet::all());
}

TEST(Error, UnsupportedDataTypeCarriesTypeAndSet) {
    UnsupportedDataTypeError e("no", DataType::Int64, DataTypeSet::floatTypes());
    EXPECT_EQ(e.offendingType(), DataType::Int64); EXPECT_EQ(e.supportedTypes(), DataTypeSet::floatTypes());
    EXPECT_EQ(e.code(), ErrorCode::UnsupportedDataType);
}
