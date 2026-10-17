// Requirements: UR-API-5, UR-EXE-5, UR-RES-12
#include <slideio/processor/core/error.hpp>

#include <gtest/gtest.h>

#include <string>

using namespace slideio::processor;

TEST(Error, MessageOnly) {
    InvalidArgumentError e("bad width");
    EXPECT_STREQ(e.what(), "bad width");
    EXPECT_EQ(e.code(), ErrorCode::InvalidArgument);
    EXPECT_STREQ(e.stageName(), "");
    EXPECT_STREQ(e.operationName(), "");
    EXPECT_STREQ(e.cause(), "");
    EXPECT_FALSE(e.hasTile());
}

TEST(Error, ContextComposesWhatInFixedOrder) {
    ErrorContext c;
    c.stageName = "nuclei";
    c.operationName = "threshold";
    c.cause = "disk full";
    c.hasTile = true;
    c.tileX = 3;
    c.tileY = 7;
    TileError e("tile failed", c);
    EXPECT_STREQ(e.what(),
                 "tile failed; stage: nuclei; operation: threshold; tile: (3, 7); cause: disk full");
    EXPECT_EQ(e.tileX(), 3);
    EXPECT_EQ(e.tileY(), 7);
    EXPECT_EQ(e.code(), ErrorCode::Tile);
}

TEST(Error, ContextStringsAreCopied) {
    std::string stage = "cells";
    ErrorContext c;
    c.stageName = stage.c_str();
    StageError e("x", c);
    stage = "zzzzz";
    EXPECT_STREQ(e.stageName(), "cells");
}

TEST(Error, SubtypeFieldsAndCodes) {
    ChannelArityError a("arity", 4);
    EXPECT_EQ(a.offendingChannelCount(), 4);
    EXPECT_EQ(a.code(), ErrorCode::ChannelArity);
    StoreFormatError s("fmt", 7, 3);
    EXPECT_EQ(s.storeVersion(), 7);
    EXPECT_EQ(s.libraryVersion(), 3);
    EXPECT_EQ(s.code(), ErrorCode::StoreFormat);
    EXPECT_EQ(StageError("s").code(), ErrorCode::Stage);
    EXPECT_EQ(CancelledError("c").code(), ErrorCode::Cancelled);
    EXPECT_EQ(IoError("i").code(), ErrorCode::Io);
    EXPECT_EQ(InternalError("n").code(), ErrorCode::Internal);
}

TEST(Error, CatchableAsProcessorErrorAndCopiesShareText) {
    try {
        throw IoError("cannot open");
    } catch (const ProcessorError& e) {
        ProcessorError copy = e;
        EXPECT_STREQ(copy.what(), "cannot open");
    }
}

TEST(Error, NullMessageReadsEmpty) {
    EXPECT_STREQ(InternalError(nullptr).what(), "");
}
