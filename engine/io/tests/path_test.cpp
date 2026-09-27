#include "engine/io/path.hpp"

#include "engine/core/error.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace orion::io {
namespace {

TEST(VirtualPath, AcceptsCookedNames) {
    for (const std::string_view text :
         {"a", "data/items.bin", "textures/stone_01.ktx2", "maps/zone-7/tile_3_4.nav",
          "a/b/c/d/e/f/g", "com10.bin", "conf/nulls.txt", "lpt/x", "0/1/2", "file.tar.zst"}) {
        EXPECT_TRUE(is_valid_virtual_path(text)) << text;
        const Result<VirtualPath> path = VirtualPath::parse(text);
        ASSERT_TRUE(path.has_value()) << text;
        EXPECT_EQ(path->view(), text);
    }
}

TEST(VirtualPath, RejectsAmbiguousOrUnportableNames) {
    const std::string too_long(kMaxVirtualPathBytes + 1, 'a');
    const std::vector<std::string_view> rejected{std::string_view{},
                                                 "/abs",
                                                 "trailing/",
                                                 "double//slash",
                                                 ".",
                                                 "..",
                                                 "a/../b",
                                                 "a/./b",
                                                 ".hidden",
                                                 "dir/.hidden",
                                                 "dot.",
                                                 "dir/dot./x",
                                                 "Upper.bin",
                                                 "back\\slash",
                                                 "space name",
                                                 "colon:name",
                                                 "tiếng_việt",
                                                 "con",
                                                 "nul.txt",
                                                 "dir/aux",
                                                 "com1.bin",
                                                 "lpt9",
                                                 "prn/x",
                                                 std::string_view(too_long)};
    for (const std::string_view text : rejected) {
        EXPECT_FALSE(is_valid_virtual_path(text)) << text;
        const Result<VirtualPath> path = VirtualPath::parse(text);
        ASSERT_FALSE(path.has_value()) << text;
        EXPECT_EQ(path.error().code(), ErrorCode::InvalidArgument);
    }
    EXPECT_FALSE(is_valid_virtual_path(std::string_view("nul\0x", 5)));
}

TEST(VirtualPath, LongestPathFits) {
    std::string longest(kMaxVirtualPathBytes, 'a');
    longest[100] = '/';
    const Result<VirtualPath> path = VirtualPath::parse(longest);
    ASSERT_TRUE(path.has_value());
    EXPECT_EQ(path->view().size(), kMaxVirtualPathBytes);
}

TEST(VirtualPath, OrdersByBytes) {
    const Result<VirtualPath> a = VirtualPath::parse("a/b");
    const Result<VirtualPath> b = VirtualPath::parse("a_b");
    const Result<VirtualPath> c = VirtualPath::parse("a/b");
    ASSERT_TRUE(a.has_value() && b.has_value() && c.has_value());
    // '/' (0x2F) đứng trước '_' (0x5F): thứ tự byte, giống thứ tự của index trong pak.
    EXPECT_LT(*a, *b);
    EXPECT_EQ(*a, *c);
}

}  // namespace
}  // namespace orion::io
