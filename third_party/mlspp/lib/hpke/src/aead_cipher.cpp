#include "aead_cipher.h"

#include <namespace.h>

#include <mbedtls/chachapoly.h>
#include <mbedtls/gcm.h>

#include <stdexcept>

namespace MLS_NAMESPACE::hpke {

///
/// ExportOnlyCipher
///
bytes
ExportOnlyCipher::seal(const bytes& /* key */,
                       const bytes& /* nonce */,
                       const bytes& /* aad */,
                       const bytes& /* pt */) const
{
  throw std::runtime_error("seal() on export-only context");
}

std::optional<bytes>
ExportOnlyCipher::open(const bytes& /* key */,
                       const bytes& /* nonce */,
                       const bytes& /* aad */,
                       const bytes& /* ct */) const
{
  throw std::runtime_error("open() on export-only context");
}

ExportOnlyCipher::ExportOnlyCipher()
  : AEAD(AEAD::ID::export_only, 0, 0)
{
}

///
/// AEADCipher
///
AEADCipher
make_aead(AEAD::ID cipher_in)
{
  return { cipher_in };
}

template<>
const AEADCipher&
AEADCipher::get<AEAD::ID::AES_128_GCM>()
{
  static const auto instance = make_aead(AEAD::ID::AES_128_GCM);
  return instance;
}

template<>
const AEADCipher&
AEADCipher::get<AEAD::ID::AES_256_GCM>()
{
  static const auto instance = make_aead(AEAD::ID::AES_256_GCM);
  return instance;
}

template<>
const AEADCipher&
AEADCipher::get<AEAD::ID::CHACHA20_POLY1305>()
{
  static const auto instance = make_aead(AEAD::ID::CHACHA20_POLY1305);
  return instance;
}

static size_t
cipher_key_size(AEAD::ID cipher)
{
  switch (cipher) {
    case AEAD::ID::AES_128_GCM:
      return 16;

    case AEAD::ID::AES_256_GCM:
    case AEAD::ID::CHACHA20_POLY1305:
      return 32;

    default:
      throw std::runtime_error("Unsupported algorithm");
  }
}

static size_t
cipher_nonce_size(AEAD::ID cipher)
{
  switch (cipher) {
    case AEAD::ID::AES_128_GCM:
    case AEAD::ID::AES_256_GCM:
    case AEAD::ID::CHACHA20_POLY1305:
      return 12;

    default:
      throw std::runtime_error("Unsupported algorithm");
  }
}

static size_t
cipher_tag_size(AEAD::ID cipher)
{
  switch (cipher) {
    case AEAD::ID::AES_128_GCM:
    case AEAD::ID::AES_256_GCM:
    case AEAD::ID::CHACHA20_POLY1305:
      return 16;

    default:
      throw std::runtime_error("Unsupported algorithm");
  }
}

AEADCipher::AEADCipher(AEAD::ID id_in)
  : AEAD(id_in, cipher_key_size(id_in), cipher_nonce_size(id_in))
  , tag_size(cipher_tag_size(id))
{
}

// AES-GCM via mbedtls_gcm.
static bytes
gcm_seal(const bytes& key,
         const bytes& nonce,
         const bytes& aad,
         const bytes& pt,
         size_t tag_size)
{
  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  if (mbedtls_gcm_setkey(&ctx,
                         MBEDTLS_CIPHER_ID_AES,
                         key.data(),
                         static_cast<unsigned int>(key.size() * 8)) != 0) {
    mbedtls_gcm_free(&ctx);
    throw std::runtime_error("gcm setkey failed");
  }

  auto ct = bytes(pt.size());
  auto tag = bytes(tag_size);
  auto rc = mbedtls_gcm_crypt_and_tag(&ctx,
                                      MBEDTLS_GCM_ENCRYPT,
                                      pt.size(),
                                      nonce.data(),
                                      nonce.size(),
                                      aad.data(),
                                      aad.size(),
                                      pt.data(),
                                      ct.data(),
                                      tag_size,
                                      tag.data());
  mbedtls_gcm_free(&ctx);
  if (rc != 0) {
    throw std::runtime_error("gcm encrypt failed");
  }

  return ct + tag;
}

static std::optional<bytes>
gcm_open(const bytes& key,
         const bytes& nonce,
         const bytes& aad,
         const bytes& ct,
         size_t tag_size)
{
  const auto inner = ct.size() - tag_size;
  auto tag = ct.slice(inner, ct.size());
  auto body = ct.slice(0, inner);

  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  if (mbedtls_gcm_setkey(&ctx,
                         MBEDTLS_CIPHER_ID_AES,
                         key.data(),
                         static_cast<unsigned int>(key.size() * 8)) != 0) {
    mbedtls_gcm_free(&ctx);
    throw std::runtime_error("gcm setkey failed");
  }

  auto pt = bytes(inner);
  auto rc = mbedtls_gcm_auth_decrypt(&ctx,
                                     inner,
                                     nonce.data(),
                                     nonce.size(),
                                     aad.data(),
                                     aad.size(),
                                     tag.data(),
                                     tag_size,
                                     body.data(),
                                     pt.data());
  mbedtls_gcm_free(&ctx);
  if (rc != 0) {
    throw std::runtime_error("AEAD authentication failure");
  }

  return pt;
}

// ChaCha20-Poly1305 via mbedtls_chachapoly (16-byte tag).
static bytes
chachapoly_seal(const bytes& key,
                const bytes& nonce,
                const bytes& aad,
                const bytes& pt)
{
  mbedtls_chachapoly_context ctx;
  mbedtls_chachapoly_init(&ctx);
  if (mbedtls_chachapoly_setkey(&ctx, key.data()) != 0) {
    mbedtls_chachapoly_free(&ctx);
    throw std::runtime_error("chachapoly setkey failed");
  }

  auto ct = bytes(pt.size());
  auto tag = bytes(16);
  auto rc = mbedtls_chachapoly_encrypt_and_tag(&ctx,
                                               pt.size(),
                                               nonce.data(),
                                               aad.data(),
                                               aad.size(),
                                               pt.data(),
                                               ct.data(),
                                               tag.data());
  mbedtls_chachapoly_free(&ctx);
  if (rc != 0) {
    throw std::runtime_error("chachapoly encrypt failed");
  }

  return ct + tag;
}

static std::optional<bytes>
chachapoly_open(const bytes& key,
                const bytes& nonce,
                const bytes& aad,
                const bytes& ct)
{
  const auto inner = ct.size() - 16;
  auto tag = ct.slice(inner, ct.size());
  auto body = ct.slice(0, inner);

  mbedtls_chachapoly_context ctx;
  mbedtls_chachapoly_init(&ctx);
  if (mbedtls_chachapoly_setkey(&ctx, key.data()) != 0) {
    mbedtls_chachapoly_free(&ctx);
    throw std::runtime_error("chachapoly setkey failed");
  }

  auto pt = bytes(inner);
  auto rc = mbedtls_chachapoly_auth_decrypt(&ctx,
                                            inner,
                                            nonce.data(),
                                            aad.data(),
                                            aad.size(),
                                            tag.data(),
                                            body.data(),
                                            pt.data());
  mbedtls_chachapoly_free(&ctx);
  if (rc != 0) {
    throw std::runtime_error("AEAD authentication failure");
  }

  return pt;
}

bytes
AEADCipher::seal(const bytes& key,
                 const bytes& nonce,
                 const bytes& aad,
                 const bytes& pt) const
{
  switch (id) {
    case AEAD::ID::AES_128_GCM:
    case AEAD::ID::AES_256_GCM:
      return gcm_seal(key, nonce, aad, pt, tag_size);
    case AEAD::ID::CHACHA20_POLY1305:
      return chachapoly_seal(key, nonce, aad, pt);
    default:
      throw std::runtime_error("Unsupported algorithm");
  }
}

std::optional<bytes>
AEADCipher::open(const bytes& key,
                 const bytes& nonce,
                 const bytes& aad,
                 const bytes& ct) const
{
  if (ct.size() < tag_size) {
    throw std::runtime_error("AEAD ciphertext smaller than tag size");
  }

  switch (id) {
    case AEAD::ID::AES_128_GCM:
    case AEAD::ID::AES_256_GCM:
      return gcm_open(key, nonce, aad, ct, tag_size);
    case AEAD::ID::CHACHA20_POLY1305:
      return chachapoly_open(key, nonce, aad, ct);
    default:
      throw std::runtime_error("Unsupported algorithm");
  }
}

} // namespace MLS_NAMESPACE::hpke
