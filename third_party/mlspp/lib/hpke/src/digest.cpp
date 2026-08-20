#include <hpke/digest.h>
#include <namespace.h>

#include <mbedtls/md.h>

#include <stdexcept>

#include "common.h"

namespace MLS_NAMESPACE::hpke {

static mbedtls_md_type_t
mbed_digest_type(Digest::ID digest)
{
  switch (digest) {
    case Digest::ID::SHA256:
      return MBEDTLS_MD_SHA256;
    case Digest::ID::SHA384:
      return MBEDTLS_MD_SHA384;
    case Digest::ID::SHA512:
      return MBEDTLS_MD_SHA512;
    default:
      // SHA3-256 (BoringSSL-only in the upstream) is not provided by mbedTLS
      // and is never exercised by DAVE ciphersuite 2.
      throw std::runtime_error("Unsupported digest algorithm");
  }
}

static const mbedtls_md_info_t*
mbed_digest_info(Digest::ID digest)
{
  const auto* info = mbedtls_md_info_from_type(mbed_digest_type(digest));
  if (info == nullptr) {
    throw std::runtime_error("Digest not available in mbedTLS build");
  }
  return info;
}

template<>
const Digest&
Digest::get<Digest::ID::SHA256>()
{
  static const Digest instance(Digest::ID::SHA256);
  return instance;
}

template<>
const Digest&
Digest::get<Digest::ID::SHA384>()
{
  static const Digest instance(Digest::ID::SHA384);
  return instance;
}

template<>
const Digest&
Digest::get<Digest::ID::SHA512>()
{
  static const Digest instance(Digest::ID::SHA512);
  return instance;
}

Digest::Digest(Digest::ID id_in)
  : id(id_in)
  , hash_size(mbedtls_md_get_size(mbed_digest_info(id_in)))
{
}

bytes
Digest::hash(const bytes& data) const
{
  auto md = bytes(hash_size);
  const auto* info = mbed_digest_info(id);
  if (mbedtls_md(info, data.data(), data.size(), md.data()) != 0) {
    throw std::runtime_error("hash failed");
  }
  return md;
}

bytes
Digest::hmac(const bytes& key, const bytes& data) const
{
  auto md = bytes(hash_size);
  const auto* info = mbed_digest_info(id);
  if (mbedtls_md_hmac(
        info, key.data(), key.size(), data.data(), data.size(), md.data()) !=
      0) {
    throw std::runtime_error("hmac failed");
  }
  return md;
}

bytes
Digest::hmac_for_hkdf_extract(const bytes& key, const bytes& data) const
{
  // mbedTLS imposes no FIPS minimum-key-length constraint, so unlike the
  // OpenSSL path this is a plain HMAC. Guard against a null key pointer, which
  // HKDF-Extract can pass for a zero-length salt.
  const auto* info = mbed_digest_info(id);
  static const uint8_t empty = 0;
  const auto* key_data = key.data();
  if (key_data == nullptr) {
    key_data = &empty;
  }

  auto md = bytes(hash_size);
  if (mbedtls_md_hmac(
        info, key_data, key.size(), data.data(), data.size(), md.data()) != 0) {
    throw std::runtime_error("hmac failed");
  }
  return md;
}

} // namespace MLS_NAMESPACE::hpke
