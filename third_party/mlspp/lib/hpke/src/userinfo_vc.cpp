#include <hpke/userinfo_vc.h>
#include <namespace.h>

#include <stdexcept>

// OpenID4VC "UserInfo" verifiable credentials are not used by DAVE (which uses
// basic/raw key credentials). credential.cpp references a few UserInfoVC symbols
// for its UserInfoVCCredential path; provide linkable stubs that throw.
namespace MLS_NAMESPACE::hpke {

struct UserInfoVC::ParsedCredential
{
};

[[noreturn]] static void
unsupported()
{
  throw std::runtime_error("UserInfo VCs not supported in this build");
}

UserInfoVC::UserInfoVC(std::string jwt)
  : parsed_cred(nullptr)
  , raw(std::move(jwt))
{
  unsupported();
}

const Signature&
UserInfoVC::signature_algorithm() const
{
  unsupported();
}

const Signature::PublicJWK&
UserInfoVC::public_key() const
{
  unsupported();
}

bool
UserInfoVC::valid_from(const Signature::PublicKey& /* issuer_key */) const
{
  unsupported();
}

} // namespace MLS_NAMESPACE::hpke
