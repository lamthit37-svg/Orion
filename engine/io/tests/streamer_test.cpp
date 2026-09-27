#include "engine/io/streamer.hpp"

#include "engine/core/error.hpp"
#include "engine/core/tests/support/alloc_hooks.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/io/file.hpp"
#include "engine/io/pak.hpp"
#include "engine/io/pak_builder.hpp"
#include "engine/io/path.hpp"
#include "engine/io/tests/support/temp_file.hpp"
#include "engine/io/vfs.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace orion::io {
namespace {

using testing::TempPath;

constexpr u32 kFileCount = 16;

[[nodiscard]] VirtualPath vpath(const std::string_view text) {
    Result<VirtualPath> path = VirtualPath::parse(text);
    EXPECT_TRUE(path.has_value()) << text;
    return *path;
}

[[nodiscard]] std::string file_name(const u32 i) {
    return "data/file_" + std::to_string(i) + ".txt";
}

[[nodiscard]] std::vector<std::byte> content_of(const u32 i) {
    const std::string text =
        "nội dung của tệp " + std::to_string(i) + std::string(usize{i} * 100U, 'x');
    const std::span<const std::byte> bytes = std::as_bytes(std::span(text));
    return {bytes.begin(), bytes.end()};
}

// Gom kết quả tới khi đủ `count`, chờ tối đa khoảng 20 giây: mất một lần đánh thức thì test hỏng
// thay vì treo.
[[nodiscard]] std::vector<StreamResult> collect(Streamer& streamer, const usize count) {
    std::vector<StreamResult> results;
    std::array<StreamResult, 4> batch{};
    for (u32 attempt = 0; attempt < 20'000 && results.size() < count; ++attempt) {
        const usize n = streamer.poll(batch);
        for (usize k = 0; k < n; ++k) {
            results.push_back(std::move(batch[k]));
        }
        if (n == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    return results;
}

class StreamerTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(crypto::initialize().has_value());
        PakBuilder builder(3);
        for (u32 i = 0; i < kFileCount; ++i) {
            ASSERT_TRUE(builder.add(vpath(file_name(i)), content_of(i)).has_value());
        }
        ASSERT_TRUE(builder.write(pak_.path()).has_value());
        Result<File> file = File::open(pak_.path());
        ASSERT_TRUE(file.has_value());
        Result<PakReader> reader = PakReader::open_unverified(std::move(*file));
        ASSERT_TRUE(reader.has_value());
        vfs_.mount_pak(std::move(*reader));
    }

    [[nodiscard]] const Vfs& vfs() const noexcept { return vfs_; }

private:
    TempPath pak_{"data.pak"};
    Vfs vfs_;
};

TEST_F(StreamerTest, ReadsEveryRequestInSubmissionOrder) {
    Result<Streamer> streamer = Streamer::start(vfs(), 32);
    ASSERT_TRUE(streamer.has_value());
    for (u32 i = 0; i < kFileCount; ++i) {
        ASSERT_TRUE(streamer->submit({.ticket = 100 + i, .path = vpath(file_name(i))}));
    }
    const std::vector<StreamResult> results = collect(*streamer, kFileCount);
    ASSERT_EQ(results.size(), kFileCount);
    for (u32 i = 0; i < kFileCount; ++i) {
        EXPECT_EQ(results[i].ticket, 100 + i);
        ASSERT_TRUE(results[i].content.has_value()) << i;
        EXPECT_EQ(*results[i].content, content_of(i));
    }
    EXPECT_EQ(streamer->pending(), 0U);
}

TEST_F(StreamerTest, MissingFileGivesNotFoundResult) {
    Result<Streamer> streamer = Streamer::start(vfs(), 4);
    ASSERT_TRUE(streamer.has_value());
    ASSERT_TRUE(streamer->submit({.ticket = 7, .path = vpath("data/missing.txt")}));
    const std::vector<StreamResult> results = collect(*streamer, 1);
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results[0].ticket, 7U);
    ASSERT_FALSE(results[0].content.has_value());
    EXPECT_EQ(results[0].content.error().code(), ErrorCode::NotFound);
}

TEST_F(StreamerTest, CapacityBoundsRequestsNotYetPolled) {
    Result<Streamer> streamer = Streamer::start(vfs(), 3);
    ASSERT_TRUE(streamer.has_value());
    for (u32 i = 0; i < 3; ++i) {
        ASSERT_TRUE(streamer->submit({.ticket = i, .path = vpath(file_name(i))}));
    }
    // Đủ 3 yêu cầu chưa poll: từ chối dù luồng IO có thể đã đọc xong.
    EXPECT_FALSE(streamer->submit({.ticket = 3, .path = vpath(file_name(3))}));
    EXPECT_EQ(streamer->pending(), 3U);
    const std::vector<StreamResult> first = collect(*streamer, 1);
    ASSERT_FALSE(first.empty());
    EXPECT_EQ(streamer->pending(), 3U - first.size());
    EXPECT_TRUE(streamer->submit({.ticket = 3, .path = vpath(file_name(3))}));
}

TEST_F(StreamerTest, ZeroCapacityMeansOne) {
    Result<Streamer> streamer = Streamer::start(vfs(), 0);
    ASSERT_TRUE(streamer.has_value());
    EXPECT_TRUE(streamer->submit({.ticket = 1, .path = vpath(file_name(1))}));
    EXPECT_FALSE(streamer->submit({.ticket = 2, .path = vpath(file_name(2))}));
    EXPECT_EQ(collect(*streamer, 1).size(), 1U);
}

// Nhiều vòng gửi rồi nhận với sức chứa nhỏ: luồng IO ngủ rồi thức hàng trăm lần. Chạy dưới TSan ở
// linux-tsan.
TEST_F(StreamerTest, ManySleepWakeCycles) {
    Result<Streamer> streamer = Streamer::start(vfs(), 2);
    ASSERT_TRUE(streamer.has_value());
    constexpr u32 kRequests = 600;
    u32 submitted = 0;
    u32 received = 0;
    u32 wrong = 0;
    std::array<StreamResult, 2> batch{};
    for (u32 attempt = 0; attempt < 200'000 && received < kRequests; ++attempt) {
        while (submitted < kRequests &&
               streamer->submit(
                   {.ticket = submitted, .path = vpath(file_name(submitted % kFileCount))})) {
            ++submitted;
        }
        const usize n = streamer->poll(batch);
        for (usize k = 0; k < n; ++k) {
            const bool ok = batch[k].ticket == received && batch[k].content.has_value() &&
                            *batch[k].content == content_of(received % kFileCount);
            wrong += ok ? 0U : 1U;
            ++received;
        }
        if (n == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    EXPECT_EQ(received, kRequests);
    EXPECT_EQ(wrong, 0U);
}

TEST_F(StreamerTest, DestroyingWithPendingRequestsStops) {
    for (u32 round = 0; round < 20; ++round) {
        Result<Streamer> streamer = Streamer::start(vfs(), 64);
        ASSERT_TRUE(streamer.has_value());
        for (u32 i = 0; i < 64; ++i) {
            ASSERT_TRUE(streamer->submit({.ticket = i, .path = vpath(file_name(i % kFileCount))}));
        }
        // Huỷ ngay: luồng IO dừng sau yêu cầu đang đọc, phần còn lại bị bỏ.
    }
}

TEST_F(StreamerTest, MovedStreamerKeepsWorkingAndSourceIsEmpty) {
    Result<Streamer> started = Streamer::start(vfs(), 4);
    ASSERT_TRUE(started.has_value());
    Streamer streamer = std::move(*started);
    ASSERT_TRUE(streamer.submit({.ticket = 5, .path = vpath(file_name(5))}));
    EXPECT_FALSE(started->submit({.ticket = 6, .path = vpath(file_name(6))}));
    std::array<StreamResult, 1> none{};
    EXPECT_EQ(started->poll(none), 0U);
    const std::vector<StreamResult> results = collect(streamer, 1);
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results[0].ticket, 5U);

    Result<Streamer> other = Streamer::start(vfs(), 4);
    ASSERT_TRUE(other.has_value());
    ASSERT_TRUE(other->submit({.ticket = 9, .path = vpath(file_name(9))}));
    // Gán đè: luồng IO của streamer cũ dừng, streamer nhận luồng của other.
    streamer = std::move(*other);
    const std::vector<StreamResult> moved = collect(streamer, 1);
    ASSERT_EQ(moved.size(), 1U);
    EXPECT_EQ(moved[0].ticket, 9U);
}

// Luồng chính gửi và nhận không cấp phát (X.7); chỉ luồng IO cấp phát nội dung tệp.
TEST_F(StreamerTest, SubmitAndPollDoNotAllocateOnTheCallingThread) {
    if (!core::testing::allocation_counting_enabled()) {
        GTEST_SKIP() << "TSan giữ operator new cho runtime của nó; không đếm được";
    }
    Result<Streamer> streamer = Streamer::start(vfs(), 8);
    ASSERT_TRUE(streamer.has_value());
    std::array<VirtualPath, kFileCount / 2> paths{
        vpath(file_name(0)), vpath(file_name(1)), vpath(file_name(2)), vpath(file_name(3)),
        vpath(file_name(4)), vpath(file_name(5)), vpath(file_name(6)), vpath(file_name(7))};
    std::array<StreamResult, 8> batch{};
    usize received = 0;
    const core::testing::AllocationScope scope;
    for (usize i = 0; i < paths.size(); ++i) {
        ASSERT_TRUE(streamer->submit({.ticket = i, .path = paths[i]}));
    }
    for (u32 attempt = 0; attempt < 20'000 && received < paths.size(); ++attempt) {
        const usize n = streamer->poll(std::span(batch).subspan(received));
        received += n;
        if (n == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    EXPECT_EQ(scope.count(), 0U);
    EXPECT_EQ(received, paths.size());
}

}  // namespace
}  // namespace orion::io
