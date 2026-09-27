// Benchmark của engine/crypto: AEAD trên cỡ gói của kênh realtime, BLAKE2b cho pak, Ed25519 cho
// token và manifest, X25519 cho bắt tay, Argon2id mức interactive. Số đo ghi trong commit kèm
// preset và máy (X.8).

#include "engine/crypto/crypto.hpp"

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/aead.hpp"
#include "engine/crypto/hash.hpp"
#include "engine/crypto/key_exchange.hpp"
#include "engine/crypto/password.hpp"
#include "engine/crypto/sign.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <vector>

namespace orion::crypto {
namespace {

[[nodiscard]] bool ready(benchmark::State& state) {
    if (!initialize().has_value()) {
        state.SkipWithError("sodium_init thất bại");
        return false;
    }
    return true;
}

// Gói 1200 byte: trần payload của một datagram trên đường truyền có MTU 1280 (IPv6 tối thiểu).
void aead_seal(benchmark::State& state) {
    if (!ready(state)) {
        return;
    }
    const auto size = static_cast<usize>(state.range(0));
    const AeadKey key = generate_aead_key();
    const std::vector<std::byte> plaintext(size, std::byte{0x42});
    const std::vector<std::byte> header(8, std::byte{0x01});
    std::vector<std::byte> out(size + kAeadTagSize);
    u64 sequence = 0;
    for ([[maybe_unused]] auto iteration : state) {
        Result<usize> written =
            seal(out, plaintext, header, nonce_from_sequence(0, sequence++), key);
        benchmark::DoNotOptimize(written);
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(static_cast<i64>(state.iterations()) * state.range(0));
}
BENCHMARK(aead_seal)->Arg(64)->Arg(1200);

void aead_open(benchmark::State& state) {
    if (!ready(state)) {
        return;
    }
    const auto size = static_cast<usize>(state.range(0));
    const AeadKey key = generate_aead_key();
    const AeadNonce nonce = nonce_from_sequence(0, 1);
    const std::vector<std::byte> plaintext(size, std::byte{0x42});
    std::vector<std::byte> sealed(size + kAeadTagSize);
    if (!seal(sealed, plaintext, {}, nonce, key).has_value()) {
        state.SkipWithError("seal thất bại");
        return;
    }
    std::vector<std::byte> out(size);
    for ([[maybe_unused]] auto iteration : state) {
        Result<usize> read = open(out, sealed, {}, nonce, key);
        benchmark::DoNotOptimize(read);
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(static_cast<i64>(state.iterations()) * state.range(0));
}
BENCHMARK(aead_open)->Arg(64)->Arg(1200);

// Khối 64 KiB như khi kiểm hash lúc mount pak.
void blake2b_hash(benchmark::State& state) {
    if (!ready(state)) {
        return;
    }
    const std::vector<std::byte> data(static_cast<usize>(state.range(0)), std::byte{0x17});
    for ([[maybe_unused]] auto iteration : state) {
        Hash digest = hash(data);
        benchmark::DoNotOptimize(digest);
    }
    state.SetBytesProcessed(static_cast<i64>(state.iterations()) * state.range(0));
}
BENCHMARK(blake2b_hash)->Arg(64)->Arg(65'536);

void ed25519_sign(benchmark::State& state) {
    if (!ready(state)) {
        return;
    }
    const SigningKeyPair pair = generate_signing_key_pair();
    const std::vector<std::byte> token(128, std::byte{0x33});
    for ([[maybe_unused]] auto iteration : state) {
        Signature signature = sign(token, pair.secret_key);
        benchmark::DoNotOptimize(signature);
    }
}
BENCHMARK(ed25519_sign);

void ed25519_verify(benchmark::State& state) {
    if (!ready(state)) {
        return;
    }
    const SigningKeyPair pair = generate_signing_key_pair();
    const std::vector<std::byte> token(128, std::byte{0x33});
    const Signature signature = sign(token, pair.secret_key);
    for ([[maybe_unused]] auto iteration : state) {
        bool valid = verify(token, signature, pair.public_key);
        benchmark::DoNotOptimize(valid);
    }
}
BENCHMARK(ed25519_verify);

// Phần của server trong một lần bắt tay: tính khoá phiên từ khoá công khai của client.
void x25519_session_keys(benchmark::State& state) {
    if (!ready(state)) {
        return;
    }
    const KeyExchangeKeyPair server = generate_key_exchange_key_pair();
    const KeyExchangeKeyPair client = generate_key_exchange_key_pair();
    for ([[maybe_unused]] auto iteration : state) {
        Result<SessionKeys> keys = server_session_keys(server, client.public_key);
        benchmark::DoNotOptimize(keys);
    }
}
BENCHMARK(x25519_session_keys);

// Một lần đăng nhập ở mức mặc định của auth: 64 MiB, 2 lượt.
void argon2id_interactive(benchmark::State& state) {
    if (!ready(state)) {
        return;
    }
    const Result<PasswordHash> stored = hash_password("benchmark", interactive_password_limits());
    if (!stored.has_value()) {
        state.SkipWithError("hash_password thất bại");
        return;
    }
    for ([[maybe_unused]] auto iteration : state) {
        bool valid = verify_password(*stored, "benchmark");
        benchmark::DoNotOptimize(valid);
    }
}
BENCHMARK(argon2id_interactive)->Unit(benchmark::kMillisecond);

}  // namespace
}  // namespace orion::crypto
