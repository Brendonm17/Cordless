#include <hpke/certificate.h>
#include <namespace.h>

#include <stdexcept>

// X.509 certificate support is not needed by DAVE (MLS ciphersuite 2 uses raw
// key-package credentials, not X.509). credential.cpp references a handful of
// Certificate symbols for its X509Credential path, so we provide linkable stubs
// that throw if that path is ever actually taken.
namespace MLS_NAMESPACE::hpke {

struct Certificate::ParsedCertificate
{
};

[[noreturn]] static void
unsupported()
{
  throw std::runtime_error("X.509 certificates not supported in this build");
}

Certificate::Certificate(const bytes& der)
  : parsed_cert(nullptr)
  , public_key(nullptr)
  , raw(der)
{
  unsupported();
}

Certificate::Certificate(const Certificate& other)
  : parsed_cert(nullptr)
  , public_key(nullptr)
  , raw(other.raw)
{
  unsupported();
}

Certificate::~Certificate() = default;

Signature::ID
Certificate::public_key_algorithm() const
{
  unsupported();
}

bool
Certificate::valid_from(const Certificate& /* parent */) const
{
  unsupported();
}

} // namespace MLS_NAMESPACE::hpke
