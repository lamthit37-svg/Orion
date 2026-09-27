#include "engine/crypto/key_exchange.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/bytes.hpp"
#include "engine/core/error.hpp"
#include "engine/crypto/aead.hpp"

#include <sodium/crypto_kx.h>
#include <sodium/crypto_scalarmult.h>

namespace orion::crypto {
namespace {

static_assert(crypto_kx_PUBLICKEYBYTES == kKeyExchangePublicKeySize);
static_assert(crypto_kx_SECRETKEYBYTES == kKeyExchangeSecretKeySize);
static_assert(crypto_kx_SESSIONKEYBYTES == kAeadKeySize);
static_assert(crypto_scalarmult_BYTES == kKeyExchangePublicKeySize);
static_assert(crypto_scalarmult_SCALARBYTES == kKeyExchangeSecretKeySize);

}  // namespace

KeyExchangeKeyPair generate_key_exchange_key_pair() noexcept {
    KeyExchangeKeyPair pair;
    const int status =
        crypto_kx_keypair(core::as_writable_uchars(pair.public_key.bytes).data(),
                          core::as_writable_uchars(pair.secret_key.mutable_view()).data());
    ORION_VERIFY(status == 0, "crypto_kx_keypair trả {}", status);
    return pair;
}

KeyExchangeKeyPair key_exchange_key_pair_from_secret(
    const KeyExchangeSecretKey& secret_key) noexcept {
    KeyExchangeKeyPair pair{{}, secret_key};
    const int status =
        crypto_scalarmult_base(core::as_writable_uchars(pair.public_key.bytes).data(),
                               core::as_uchars(secret_key.view()).data());
    ORION_VERIFY(status == 0, "crypto_scalarmult_base trả {}", status);
    return pair;
}

Result<SessionKeys> client_session_keys(const KeyExchangeKeyPair& client,
                                        const KeyExchangePublicKey& server_public_key) noexcept {
    SessionKeys keys;
    if (crypto_kx_client_session_keys(core::as_writable_uchars(keys.receive.mutable_view()).data(),
                                      core::as_writable_uchars(keys.transmit.mutable_view()).data(),
                                      core::as_uchars(client.public_key.bytes).data(),
                                      core::as_uchars(client.secret_key.view()).data(),
                                      core::as_uchars(server_public_key.bytes).data()) != 0) {
        return fail(ErrorCode::InvalidArgument, "kx: khoá công khai của server không hợp lệ");
    }
    return keys;
}

Result<SessionKeys> server_session_keys(const KeyExchangeKeyPair& server,
                                        const KeyExchangePublicKey& client_public_key) noexcept {
    SessionKeys keys;
    if (crypto_kx_server_session_keys(core::as_writable_uchars(keys.receive.mutable_view()).data(),
                                      core::as_writable_uchars(keys.transmit.mutable_view()).data(),
                                      core::as_uchars(server.public_key.bytes).data(),
                                      core::as_uchars(server.secret_key.view()).data(),
                                      core::as_uchars(client_public_key.bytes).data()) != 0) {
        return fail(ErrorCode::InvalidArgument, "kx: khoá công khai của client không hợp lệ");
    }
    return keys;
}

}  // namespace orion::crypto
