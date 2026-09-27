#pragma once

// AEAD ChaCha20-Poly1305 bản IETF (RFC 8439) cho từng gói của kênh realtime (X.9). Chạy bằng phần
// mềm thuần trên mọi CPU, không cần lệnh AES như AES-GCM.
//
// Nonce 96 bit lấy từ số thứ tự gói (X.9): mỗi cặp (khoá, nonce) chỉ được dùng một lần. Hai chiều
// của một kết nối dùng hai khoá khác nhau (key_exchange.hpp), nên số thứ tự của hai chiều không
// đụng nhau.

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"

#include <cstddef>
#include <span>

namespace orion::crypto {

inline constexpr usize kAeadKeySize = 32;
inline constexpr usize kAeadNonceSize = 12;
inline constexpr usize kAeadTagSize = 16;

struct AeadKeyTag;
struct AeadNonceTag;
using AeadKey = SecretBytes<kAeadKeySize, AeadKeyTag>;
using AeadNonce = PublicBytes<kAeadNonceSize, AeadNonceTag>;

[[nodiscard]] AeadKey generate_aead_key() noexcept;

// 4 byte `channel` rồi 8 byte `sequence`, little-endian. `channel` tách các dòng gói dùng chung một
// khoá (ví dụ theo kênh của protocol); `sequence` không bao giờ lặp lại trong một dòng.
[[nodiscard]] AeadNonce nonce_from_sequence(u32 channel, u64 sequence) noexcept;

// Mã hoá và xác thực plaintext cùng associated_data (xác thực nhưng không mã hoá, ví dụ header của
// gói). Ghi plaintext.size() + kAeadTagSize byte vào out và trả số đó; out thiếu chỗ trả
// InvalidArgument. out có thể bắt đầu đúng ở plaintext (mã hoá tại chỗ, test của libsodium cũng
// dùng cách này), nhưng không được chồng lệch một phần.
[[nodiscard]] Result<usize> seal(std::span<std::byte> out, std::span<const std::byte> plaintext,
                                 std::span<const std::byte> associated_data, const AeadNonce& nonce,
                                 const AeadKey& key) noexcept;

// Ngược của seal: kiểm tag trước rồi mới giải mã, trả số byte plaintext. Dữ liệu từ mạng đi thẳng
// vào được (X.5): gói ngắn hơn tag hay sai tag trả DataLoss, không bao giờ UB. Khi sai tag,
// libsodium ghi 0 lên ciphertext.size() - kAeadTagSize byte đầu của out, nên out không bao giờ chứa
// plaintext chưa xác thực. Mở tại chỗ như seal.
[[nodiscard]] Result<usize> open(std::span<std::byte> out, std::span<const std::byte> ciphertext,
                                 std::span<const std::byte> associated_data, const AeadNonce& nonce,
                                 const AeadKey& key) noexcept;

}  // namespace orion::crypto
