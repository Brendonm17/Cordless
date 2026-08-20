#pragma once

#include <hpke/digest.h>
#include <hpke/hpke.h>
#include <hpke/signature.h>
#include <namespace.h>

namespace MLS_NAMESPACE::hpke {

// RSA is not used by MLS ciphersuite 2 (DAVE) and only appears in X.509
// credential paths, which this build stubs out. RSASignature constructs so the
// Signature::get<RSA_*> specializations link, but every operation throws.
struct RSASignature : public Signature
{
  explicit RSASignature(Digest::ID digest)
    : Signature(digest_to_sig(digest))
  {
  }

  std::unique_ptr<Signature::PrivateKey> generate_key_pair() const override;
  std::unique_ptr<Signature::PrivateKey> derive_key_pair(
    const bytes& ikm) const override;

  static std::unique_ptr<Signature::PrivateKey> generate_key_pair(size_t bits);

  bytes serialize(const Signature::PublicKey& pk) const override;
  std::unique_ptr<Signature::PublicKey> deserialize(
    const bytes& enc) const override;

  bytes serialize_private(const Signature::PrivateKey& sk) const override;
  std::unique_ptr<Signature::PrivateKey> deserialize_private(
    const bytes& skm) const override;

  bytes sign(const bytes& data, const Signature::PrivateKey& sk) const override;
  bool verify(const bytes& data,
              const bytes& sig,
              const Signature::PublicKey& pk) const override;

  std::unique_ptr<Signature::PrivateKey> import_jwk_private(
    const std::string& json_str) const override;
  std::unique_ptr<Signature::PublicKey> import_jwk(
    const std::string& json_str) const override;
  std::string export_jwk_private(
    const Signature::PrivateKey& sk) const override;
  std::string export_jwk(const Signature::PublicKey& pk) const override;

private:
  static Signature::ID digest_to_sig(Digest::ID digest);
};

} // namespace MLS_NAMESPACE::hpke
