#include "rsa.h"

#include <namespace.h>

#include <stdexcept>

namespace MLS_NAMESPACE::hpke {

Signature::ID
RSASignature::digest_to_sig(Digest::ID digest)
{
  switch (digest) {
    case Digest::ID::SHA256:
      return Signature::ID::RSA_SHA256;
    case Digest::ID::SHA384:
      return Signature::ID::RSA_SHA384;
    case Digest::ID::SHA512:
      return Signature::ID::RSA_SHA512;
    default:
      throw std::runtime_error("Unsupported digest for RSA");
  }
}

[[noreturn]] static void
unsupported()
{
  throw std::runtime_error("RSA signatures not supported in this build");
}

std::unique_ptr<Signature::PrivateKey>
RSASignature::generate_key_pair() const
{
  unsupported();
}

std::unique_ptr<Signature::PrivateKey>
RSASignature::derive_key_pair(const bytes& /* ikm */) const
{
  unsupported();
}

std::unique_ptr<Signature::PrivateKey>
RSASignature::generate_key_pair(size_t /* bits */)
{
  unsupported();
}

bytes
RSASignature::serialize(const Signature::PublicKey& /* pk */) const
{
  unsupported();
}

std::unique_ptr<Signature::PublicKey>
RSASignature::deserialize(const bytes& /* enc */) const
{
  unsupported();
}

bytes
RSASignature::serialize_private(const Signature::PrivateKey& /* sk */) const
{
  unsupported();
}

std::unique_ptr<Signature::PrivateKey>
RSASignature::deserialize_private(const bytes& /* skm */) const
{
  unsupported();
}

bytes
RSASignature::sign(const bytes& /* data */,
                   const Signature::PrivateKey& /* sk */) const
{
  unsupported();
}

bool
RSASignature::verify(const bytes& /* data */,
                     const bytes& /* sig */,
                     const Signature::PublicKey& /* pk */) const
{
  unsupported();
}

std::unique_ptr<Signature::PrivateKey>
RSASignature::import_jwk_private(const std::string& /* json_str */) const
{
  unsupported();
}

std::unique_ptr<Signature::PublicKey>
RSASignature::import_jwk(const std::string& /* json_str */) const
{
  unsupported();
}

std::string
RSASignature::export_jwk_private(const Signature::PrivateKey& /* sk */) const
{
  unsupported();
}

std::string
RSASignature::export_jwk(const Signature::PublicKey& /* pk */) const
{
  unsupported();
}

} // namespace MLS_NAMESPACE::hpke
