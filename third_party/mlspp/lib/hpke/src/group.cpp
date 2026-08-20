// mbedTLS-backed replacement for the OpenSSL EC group implementation. Only the
// NIST prime curves (P-256/384/521) are functional; DAVE ciphersuite 2 uses
// P-256 exclusively. The raw/Edwards curves (X25519/X448/Ed25519/Ed448) are
// referenced by dhkem.cpp/signature.cpp template specializations but never
// exercised by cs2, so they construct fine and throw on use.
//
// MBEDTLS_ALLOW_PRIVATE_ACCESS is mbedTLS's sanctioned mechanism for reaching
// struct internals (the group generator/order and keypair fields); it must be
// defined before any mbedTLS header is included.
#define MBEDTLS_ALLOW_PRIVATE_ACCESS

#include "group.h"

#include <hpke/random.h>
#include <namespace.h>

#include "common.h"
#include "mbed_rng.h"

#include <mbedtls/ecdh.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/md.h>

#include <stdexcept>

namespace MLS_NAMESPACE::hpke {

///
/// Size / metadata tables (crypto-backend independent)
///

static inline size_t
group_seed_size(Group::ID group_id)
{
  switch (group_id) {
    case Group::ID::P256:
      return 128;
    case Group::ID::P384:
      return 48;
    case Group::ID::P521:
      return 66;
    case Group::ID::X25519:
      return 32;
    case Group::ID::X448:
      return 56;
    case Group::ID::Ed25519:
    case Group::ID::Ed448:
      return 0;
    default:
      throw std::runtime_error("Unknown group");
  }
}

static inline size_t
group_dh_size(Group::ID group_id)
{
  switch (group_id) {
    case Group::ID::P256:
      return 32;
    case Group::ID::P384:
      return 48;
    case Group::ID::P521:
      return 66;
    case Group::ID::X25519:
      return 32;
    case Group::ID::X448:
      return 56;
    case Group::ID::Ed25519:
    case Group::ID::Ed448:
      return 0;
    default:
      throw std::runtime_error("Unknown group");
  }
}

static inline size_t
group_pk_size(Group::ID group_id)
{
  switch (group_id) {
    case Group::ID::P256:
      return 65;
    case Group::ID::P384:
      return 97;
    case Group::ID::P521:
      return 133;
    case Group::ID::X25519:
    case Group::ID::Ed25519:
      return 32;
    case Group::ID::X448:
      return 56;
    case Group::ID::Ed448:
      return 57;
    default:
      throw std::runtime_error("Unknown group");
  }
}

static inline size_t
group_sk_size(Group::ID group_id)
{
  switch (group_id) {
    case Group::ID::P256:
      return 32;
    case Group::ID::P384:
      return 48;
    case Group::ID::P521:
      return 66;
    case Group::ID::X25519:
    case Group::ID::Ed25519:
      return 32;
    case Group::ID::X448:
      return 56;
    case Group::ID::Ed448:
      return 57;
    default:
      throw std::runtime_error("Unknown group");
  }
}

static inline std::string
group_jwk_curve_name(Group::ID group_id)
{
  switch (group_id) {
    case Group::ID::P256:
      return "P-256";
    case Group::ID::P384:
      return "P-384";
    case Group::ID::P521:
      return "P-521";
    case Group::ID::Ed25519:
      return "Ed25519";
    case Group::ID::Ed448:
      return "Ed448";
    case Group::ID::X25519:
      return "X25519";
    case Group::ID::X448:
      return "X448";
    default:
      throw std::runtime_error("Unknown group");
  }
}

static inline std::string
group_jwk_key_type(Group::ID group_id)
{
  switch (group_id) {
    case Group::ID::P256:
    case Group::ID::P384:
    case Group::ID::P521:
      return "EC";
    case Group::ID::Ed25519:
    case Group::ID::Ed448:
    case Group::ID::X25519:
    case Group::ID::X448:
      return "OKP";
    default:
      throw std::runtime_error("Unknown group");
  }
}

Group::Group(ID group_id_in, const KDF& kdf_in)
  : id(group_id_in)
  , seed_size(group_seed_size(group_id_in))
  , dh_size(group_dh_size(group_id_in))
  , pk_size(group_pk_size(group_id_in))
  , sk_size(group_sk_size(group_id_in))
  , jwk_key_type(group_jwk_key_type(group_id_in))
  , jwk_curve_name(group_jwk_curve_name(group_id_in))
  , kdf(kdf_in)
{
}

///
/// NIST prime-curve implementation on mbedTLS
///

namespace {

struct ECPublicKey : public Group::PublicKey
{
  bytes data; // uncompressed SEC1 point (0x04 || X || Y), pk_size bytes
  explicit ECPublicKey(bytes data_in)
    : data(std::move(data_in))
  {
  }
  ~ECPublicKey() override = default;
};

struct ECPrivateKey : public Group::PrivateKey
{
  bytes d;   // scalar, sk_size bytes big-endian
  bytes pub; // matching public point, pk_size bytes
  ECPrivateKey(bytes d_in, bytes pub_in)
    : d(std::move(d_in))
    , pub(std::move(pub_in))
  {
  }
  ~ECPrivateKey() override = default;

  std::unique_ptr<Group::PublicKey> public_key() const override
  {
    return std::make_unique<ECPublicKey>(pub);
  }
};

// RAII guards for the mbedTLS primitives.
struct Grp
{
  mbedtls_ecp_group g;
  explicit Grp(mbedtls_ecp_group_id id)
  {
    mbedtls_ecp_group_init(&g);
    if (mbedtls_ecp_group_load(&g, id) != 0) {
      throw std::runtime_error("ecp group load failed");
    }
  }
  ~Grp() { mbedtls_ecp_group_free(&g); }
};

struct Mpi
{
  mbedtls_mpi v;
  Mpi() { mbedtls_mpi_init(&v); }
  ~Mpi() { mbedtls_mpi_free(&v); }
};

struct Point
{
  mbedtls_ecp_point v;
  Point() { mbedtls_ecp_point_init(&v); }
  ~Point() { mbedtls_ecp_point_free(&v); }
};

} // namespace

struct ECKeyGroup : public Group
{
  ECKeyGroup(Group::ID group_id, const KDF& kdf_in)
    : Group(group_id, kdf_in)
    , curve(group_to_curve(group_id))
    , md(group_to_md(group_id))
  {
  }

  std::unique_ptr<Group::PrivateKey> generate_key_pair() const override
  {
    Grp grp(curve);
    Mpi d;
    Point Q;
    if (mbedtls_ecp_gen_keypair(&grp.g, &d.v, &Q.v, mbed_rng, nullptr) != 0) {
      throw std::runtime_error("ecp keygen failed");
    }
    return make_priv(grp, d, Q);
  }

  std::unique_ptr<Group::PrivateKey> derive_key_pair(
    const bytes& suite_id,
    const bytes& ikm) const override
  {
    static const int retry_limit = 255;
    static const auto label_dkp_prk = from_ascii("dkp_prk");
    static const auto label_candidate = from_ascii("candidate");

    auto dkp_prk = kdf.labeled_extract(suite_id, {}, label_dkp_prk, ikm);

    Grp grp(curve);
    for (auto counter = 0; counter <= retry_limit; counter++) {
      auto ctr = i2osp(counter, 1);
      auto candidate =
        kdf.labeled_expand(suite_id, dkp_prk, label_candidate, ctr, sk_size);
      candidate.at(0) &= bitmask();

      Mpi d;
      if (mbedtls_mpi_read_binary(&d.v, candidate.data(), candidate.size()) !=
          0) {
        continue;
      }
      // check_privkey enforces 1 <= d <= N-2 for the curve.
      if (mbedtls_ecp_check_privkey(&grp.g, &d.v) != 0) {
        continue;
      }

      Point Q;
      if (mbedtls_ecp_mul(&grp.g, &Q.v, &d.v, &grp.g.G, mbed_rng, nullptr) !=
          0) {
        throw std::runtime_error("ecp_mul failed");
      }
      return make_priv(grp, d, Q);
    }

    throw std::runtime_error("DeriveKeyPair iteration limit exceeded");
  }

  std::unique_ptr<Group::PrivateKey> random_scalar(
    const bytes& seed) const override
  {
    Grp grp(curve);
    auto start = size_t(0);
    while (start + sk_size <= seed.size()) {
      auto candidate = seed.slice(start, start + sk_size);
      Mpi d;
      if (mbedtls_mpi_read_binary(&d.v, candidate.data(), candidate.size()) ==
            0 &&
          mbedtls_ecp_check_privkey(&grp.g, &d.v) == 0) {
        Point Q;
        if (mbedtls_ecp_mul(&grp.g, &Q.v, &d.v, &grp.g.G, mbed_rng, nullptr) !=
            0) {
          throw std::runtime_error("ecp_mul failed");
        }
        return make_priv(grp, d, Q);
      }
      start += sk_size;
    }
    throw std::runtime_error("Rejection sampling failed");
  }

  bytes serialize(const Group::PublicKey& pk) const override
  {
    return dynamic_cast<const ECPublicKey&>(pk).data;
  }

  std::unique_ptr<Group::PublicKey> deserialize(const bytes& enc) const override
  {
    Grp grp(curve);
    Point Q;
    if (mbedtls_ecp_point_read_binary(&grp.g, &Q.v, enc.data(), enc.size()) !=
          0 ||
        mbedtls_ecp_check_pubkey(&grp.g, &Q.v) != 0) {
      throw std::runtime_error("Unable to deserialize public key");
    }
    return std::make_unique<ECPublicKey>(enc);
  }

  bytes serialize_private(const Group::PrivateKey& sk) const override
  {
    return dynamic_cast<const ECPrivateKey&>(sk).d;
  }

  std::unique_ptr<Group::PrivateKey> deserialize_private(
    const bytes& skm) const override
  {
    Grp grp(curve);
    Mpi d;
    if (mbedtls_mpi_read_binary(&d.v, skm.data(), skm.size()) != 0 ||
        mbedtls_ecp_check_privkey(&grp.g, &d.v) != 0) {
      throw std::runtime_error("Unable to deserialize private key");
    }
    Point Q;
    if (mbedtls_ecp_mul(&grp.g, &Q.v, &d.v, &grp.g.G, mbed_rng, nullptr) != 0) {
      throw std::runtime_error("ecp_mul failed");
    }
    return make_priv(grp, d, Q);
  }

  std::unique_ptr<Group::PrivateKey> deserialize_private_der(
    const bytes& /* der */) const override
  {
    throw std::runtime_error("DER private key import not supported");
  }

  bytes dh(const Group::PrivateKey& sk,
           const Group::PublicKey& pk) const override
  {
    const auto& rsk = dynamic_cast<const ECPrivateKey&>(sk);
    const auto& rpk = dynamic_cast<const ECPublicKey&>(pk);

    Grp grp(curve);
    Mpi d;
    Point Q;
    Mpi z;
    if (mbedtls_mpi_read_binary(&d.v, rsk.d.data(), rsk.d.size()) != 0 ||
        mbedtls_ecp_point_read_binary(
          &grp.g, &Q.v, rpk.data.data(), rpk.data.size()) != 0) {
      throw std::runtime_error("dh input decode failed");
    }
    if (mbedtls_ecdh_compute_shared(
          &grp.g, &z.v, &Q.v, &d.v, mbed_rng, nullptr) != 0) {
      throw std::runtime_error("ecdh failed");
    }

    auto out = bytes(dh_size);
    if (mbedtls_mpi_write_binary(&z.v, out.data(), out.size()) != 0) {
      throw std::runtime_error("dh output encode failed");
    }
    return out;
  }

  bytes sign(const bytes& data, const Group::PrivateKey& sk) const override
  {
    const auto& rsk = dynamic_cast<const ECPrivateKey&>(sk);

    auto hash = digest(data);

    mbedtls_ecdsa_context ctx;
    mbedtls_ecdsa_init(&ctx);
    auto sig = bytes(MBEDTLS_ECDSA_MAX_LEN);
    size_t slen = 0;
    auto rc = mbedtls_ecp_read_key(curve, &ctx, rsk.d.data(), rsk.d.size());
    if (rc == 0) {
      rc = mbedtls_ecdsa_write_signature(&ctx,
                                         md,
                                         hash.data(),
                                         hash.size(),
                                         sig.data(),
                                         sig.size(),
                                         &slen,
                                         mbed_rng,
                                         nullptr);
    }
    mbedtls_ecdsa_free(&ctx);
    if (rc != 0) {
      throw std::runtime_error("ecdsa sign failed");
    }
    sig.resize(slen);
    return sig;
  }

  bool verify(const bytes& data,
              const bytes& sig,
              const Group::PublicKey& pk) const override
  {
    const auto& rpk = dynamic_cast<const ECPublicKey&>(pk);
    auto hash = digest(data);

    Grp grp(curve);
    Point Q;
    if (mbedtls_ecp_point_read_binary(
          &grp.g, &Q.v, rpk.data.data(), rpk.data.size()) != 0) {
      return false;
    }

    mbedtls_ecdsa_context ctx;
    mbedtls_ecdsa_init(&ctx);
    auto rc = mbedtls_ecp_set_public_key(curve, &ctx, &Q.v);
    if (rc == 0) {
      rc = mbedtls_ecdsa_read_signature(
        &ctx, hash.data(), hash.size(), sig.data(), sig.size());
    }
    mbedtls_ecdsa_free(&ctx);
    return rc == 0;
  }

  std::tuple<bytes, bytes> coordinates(
    const Group::PublicKey& pk) const override
  {
    // Uncompressed point is 0x04 || X || Y, each coordinate dh_size bytes.
    const auto& data = dynamic_cast<const ECPublicKey&>(pk).data;
    if (data.size() != 1 + 2 * dh_size || data.at(0) != 0x04) {
      throw std::runtime_error("Unexpected public key encoding");
    }
    auto x = data.slice(1, 1 + dh_size);
    auto y = data.slice(1 + dh_size, data.size());
    return { x, y };
  }

  std::unique_ptr<Group::PublicKey> public_key_from_coordinates(
    const bytes& x,
    const bytes& y) const override
  {
    if (x.size() != dh_size || y.size() != dh_size) {
      throw std::runtime_error("Invalid coordinate size");
    }
    auto enc = bytes{ 0x04 } + x + y;
    return deserialize(enc);
  }

private:
  const mbedtls_ecp_group_id curve;
  const mbedtls_md_type_t md;

  static std::unique_ptr<Group::PrivateKey> make_priv(Grp& grp,
                                                      Mpi& d,
                                                      Point& Q)
  {
    // Serialize d (left-padded) and Q (uncompressed) into stable byte forms.
    const auto sk_size = mbedtls_mpi_size(&grp.g.N);
    auto dbuf = bytes(sk_size);
    if (mbedtls_mpi_write_binary(&d.v, dbuf.data(), dbuf.size()) != 0) {
      throw std::runtime_error("scalar encode failed");
    }

    auto pbuf = bytes(2 * mbedtls_mpi_size(&grp.g.P) + 1);
    size_t olen = 0;
    if (mbedtls_ecp_point_write_binary(&grp.g,
                                       &Q.v,
                                       MBEDTLS_ECP_PF_UNCOMPRESSED,
                                       &olen,
                                       pbuf.data(),
                                       pbuf.size()) != 0) {
      throw std::runtime_error("point encode failed");
    }
    pbuf.resize(olen);
    return std::make_unique<ECPrivateKey>(std::move(dbuf), std::move(pbuf));
  }

  bytes digest(const bytes& data) const
  {
    const auto* info = mbedtls_md_info_from_type(md);
    auto out = bytes(mbedtls_md_get_size(info));
    if (mbedtls_md(info, data.data(), data.size(), out.data()) != 0) {
      throw std::runtime_error("hash failed");
    }
    return out;
  }

  uint8_t bitmask() const
  {
    switch (id) {
      case Group::ID::P256:
      case Group::ID::P384:
        return 0xff;
      case Group::ID::P521:
        return 0x01;
      default:
        throw std::runtime_error("Unsupported algorithm");
    }
  }

  static mbedtls_ecp_group_id group_to_curve(Group::ID group_id)
  {
    switch (group_id) {
      case Group::ID::P256:
        return MBEDTLS_ECP_DP_SECP256R1;
      case Group::ID::P384:
        return MBEDTLS_ECP_DP_SECP384R1;
      case Group::ID::P521:
        return MBEDTLS_ECP_DP_SECP521R1;
      default:
        throw std::runtime_error("Unsupported algorithm");
    }
  }

  static mbedtls_md_type_t group_to_md(Group::ID group_id)
  {
    switch (group_id) {
      case Group::ID::P256:
        return MBEDTLS_MD_SHA256;
      case Group::ID::P384:
        return MBEDTLS_MD_SHA384;
      case Group::ID::P521:
        return MBEDTLS_MD_SHA512;
      default:
        throw std::runtime_error("Unsupported algorithm");
    }
  }
};

///
/// Placeholder for the raw/Edwards curves DAVE ciphersuite 2 never uses. It
/// constructs (so the template specializations link) but throws on any actual
/// operation, since mbedTLS 3.6 does not provide EdDSA and DAVE never needs it.
///

struct UnsupportedGroup : public Group
{
  UnsupportedGroup(Group::ID group_id, const KDF& kdf_in)
    : Group(group_id, kdf_in)
  {
  }

  [[noreturn]] static void die()
  {
    throw std::runtime_error("Group not supported in this build (cs2 only)");
  }

  std::unique_ptr<Group::PrivateKey> generate_key_pair() const override
  {
    die();
  }
  std::unique_ptr<Group::PrivateKey> derive_key_pair(
    const bytes&,
    const bytes&) const override
  {
    die();
  }
  std::unique_ptr<Group::PrivateKey> random_scalar(
    const bytes&) const override
  {
    die();
  }
  bytes serialize(const Group::PublicKey&) const override { die(); }
  std::unique_ptr<Group::PublicKey> deserialize(const bytes&) const override
  {
    die();
  }
  bytes serialize_private(const Group::PrivateKey&) const override { die(); }
  std::unique_ptr<Group::PrivateKey> deserialize_private(
    const bytes&) const override
  {
    die();
  }
  std::unique_ptr<Group::PrivateKey> deserialize_private_der(
    const bytes&) const override
  {
    die();
  }
  bytes dh(const Group::PrivateKey&, const Group::PublicKey&) const override
  {
    die();
  }
  bytes sign(const bytes&, const Group::PrivateKey&) const override { die(); }
  bool verify(const bytes&,
              const bytes&,
              const Group::PublicKey&) const override
  {
    die();
  }
  std::tuple<bytes, bytes> coordinates(
    const Group::PublicKey&) const override
  {
    die();
  }
  std::unique_ptr<Group::PublicKey> public_key_from_coordinates(
    const bytes&,
    const bytes&) const override
  {
    die();
  }
};

///
/// Factories
///

template<>
const Group&
Group::get<Group::ID::P256>()
{
  static const ECKeyGroup instance(Group::ID::P256,
                                   KDF::get<KDF::ID::HKDF_SHA256>());
  return instance;
}

template<>
const Group&
Group::get<Group::ID::P384>()
{
  static const ECKeyGroup instance(Group::ID::P384,
                                   KDF::get<KDF::ID::HKDF_SHA384>());
  return instance;
}

template<>
const Group&
Group::get<Group::ID::P521>()
{
  static const ECKeyGroup instance(Group::ID::P521,
                                   KDF::get<KDF::ID::HKDF_SHA512>());
  return instance;
}

template<>
const Group&
Group::get<Group::ID::X25519>()
{
  static const UnsupportedGroup instance(Group::ID::X25519,
                                         KDF::get<KDF::ID::HKDF_SHA256>());
  return instance;
}

template<>
const Group&
Group::get<Group::ID::X448>()
{
  static const UnsupportedGroup instance(Group::ID::X448,
                                         KDF::get<KDF::ID::HKDF_SHA512>());
  return instance;
}

template<>
const Group&
Group::get<Group::ID::Ed25519>()
{
  static const UnsupportedGroup instance(Group::ID::Ed25519,
                                         KDF::get<KDF::ID::HKDF_SHA256>());
  return instance;
}

template<>
const Group&
Group::get<Group::ID::Ed448>()
{
  static const UnsupportedGroup instance(Group::ID::Ed448,
                                         KDF::get<KDF::ID::HKDF_SHA512>());
  return instance;
}

} // namespace MLS_NAMESPACE::hpke
