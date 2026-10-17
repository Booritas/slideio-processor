// Requirements: UR-IMG-6, UR-IMG-7, UR-ALG-2
#include <slideio/processor/core/channel_selection.hpp>
#include <slideio/processor/core/error.hpp>

#include <gtest/gtest.h>

#include <utility>

using namespace slideio::processor;

TEST(ChannelSelection, ExplicitOrdered) {
    ChannelSelection s = ChannelSelection::single(3); s.add(0);
    EXPECT_EQ(s.count(), 2); EXPECT_EQ(s.at(0), 3); EXPECT_EQ(s.at(1), 0);
    EXPECT_THROW(s.add(3), InvalidArgumentError);
    EXPECT_THROW(s.add(-1), InvalidArgumentError);
    EXPECT_THROW(s.at(2), InvalidArgumentError);
    EXPECT_EQ(ChannelSelection::range(2, 3).at(2), 4);
    EXPECT_THROW(ChannelSelection::range(0, 0), InvalidArgumentError);
}
TEST(ChannelSelection, AllAndEmpty) {
    auto a = ChannelSelection::all(); EXPECT_TRUE(a.isAll()); EXPECT_EQ(a.count(), 0);
    EXPECT_TRUE(a.contains(57)); EXPECT_THROW(a.add(1), InvalidArgumentError);
    ChannelSelection e; EXPECT_FALSE(e.isAll()); EXPECT_FALSE(e.contains(0));
    ChannelSelection c = ChannelSelection::single(1); ChannelSelection d(c); c.add(2);
    EXPECT_EQ(d.count(), 1);
}
TEST(ChannelSelection, RejectsBadArguments) {
    EXPECT_THROW(ChannelSelection::single(-1), InvalidArgumentError);
    EXPECT_THROW(ChannelSelection::range(-1, 2), InvalidArgumentError);
    EXPECT_THROW(ChannelSelection::range(0, -3), InvalidArgumentError);
    EXPECT_THROW(ChannelSelection::all().at(0), InvalidArgumentError);
    EXPECT_FALSE(ChannelSelection::all().contains(-1));
}
TEST(ChannelSelection, CopyAndMove) {
    ChannelSelection s = ChannelSelection::range(1, 3);
    ChannelSelection c; c = s;
    ChannelSelection m(std::move(s));
    EXPECT_EQ(c.count(), 3); EXPECT_EQ(m.at(2), 3);
    ChannelSelection n; n = std::move(m);
    EXPECT_EQ(n.count(), 3);
    EXPECT_EQ(s.count(), 0);                 // moved-from is valid and empty
    s = c; EXPECT_EQ(s.at(0), 1);
}
TEST(ChannelSelection, AddChainsInOrder) {
    ChannelSelection s = ChannelSelection::single(1);
    ChannelSelection& r = s.add(2).add(3);
    EXPECT_EQ(&r, &s);
    EXPECT_EQ(s.count(), 3);
    EXPECT_EQ(s.at(0), 1); EXPECT_EQ(s.at(1), 2); EXPECT_EQ(s.at(2), 3);
}
TEST(ChannelSelection, RangeLimitsAndAllIsNoexcept) {
    static_assert(noexcept(ChannelSelection::all()), "all() must not throw");
    EXPECT_THROW(ChannelSelection::range(2147483647, 2), InvalidArgumentError);
    EXPECT_THROW(ChannelSelection::range(2, 2147483647), InvalidArgumentError);
    ChannelSelection top = ChannelSelection::range(2147483646, 2);
    EXPECT_EQ(top.at(1), 2147483647);
    ChannelSelection a = ChannelSelection::all(), b(a), c; c = a;
    EXPECT_TRUE(b.isAll()); EXPECT_TRUE(c.isAll());
    ChannelSelection m(std::move(a));
    EXPECT_TRUE(m.isAll()); EXPECT_FALSE(a.isAll()); EXPECT_EQ(a.count(), 0);
}
