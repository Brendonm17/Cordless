// C++ bridge to the mlspp MLS engine + the mbedTLS HPKE shim, exposed to the
// plain-C voice gateway through the extern "C" surface in dave.h.

#include "voice/dave.h"
#include "voice/dave_frame.h"

#include <mls/crypto.h>
#include <mls/core_types.h>
#include <mls/credential.h>
#include <mls/messages.h>
#include <mls/key_schedule.h>
#include <mls/state.h>
#include <tls/tls_syntax.h>

#include <hpke/hpke.h>
#include <hpke/random.h>
#include <hpke/signature.h>

#include <bytes/bytes.h>

#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <optional>
#include <set>
#include <string>
#include <vector>

extern "C" void console_logf(const char *fmt, ...);
extern "C" void console_dbgf(const char *fmt, ...);

namespace h = mlspp::hpke;
using mlspp::bytes_ns::bytes;
using mlspp::bytes_ns::from_ascii;


static int
selftest_hpke()
{
  int fails = 0;

  // 1. ECDSA-P256 sign / verify, including tamper rejection.
  const auto& sig = h::Signature::get<h::Signature::ID::P256_SHA256>();
  auto sk = sig.generate_key_pair();
  auto pk = sk->public_key();
  auto msg = from_ascii("cordless dave selftest message");
  auto signature = sig.sign(msg, *sk);
  if (!sig.verify(msg, signature, *pk)) {
    fails |= 1;
  }
  auto tampered = msg;
  tampered.at(0) ^= 0xFF;
  if (sig.verify(tampered, signature, *pk)) {
    fails |= 2;  // a modified message must not verify
  }

  // 2. DHKEM-P256 encap / decap must agree on the shared secret.
  const auto& kem = h::KEM::get<h::KEM::ID::DHKEM_P256_SHA256>();
  auto skR = kem.derive_key_pair(h::random_bytes(32));
  auto pkR = skR->public_key();
  auto [ss_s, enc] = kem.encap(*pkR);
  auto ss_r = kem.decap(enc, *skR);
  if (ss_s != ss_r) {
    fails |= 4;
  }

  // 3. Full HPKE base-mode seal/open.
  auto hpke =
    h::HPKE(h::KEM::ID::DHKEM_P256_SHA256, h::KDF::ID::HKDF_SHA256,
            h::AEAD::ID::AES_128_GCM);
  auto info = from_ascii("cordless-dave");
  auto sender = hpke.setup_base_s(*pkR, info);
  auto receiver = hpke.setup_base_r(sender.first, *skR, info);
  auto aad = from_ascii("frame-aad");
  auto pt = from_ascii("end-to-end encrypted voice");
  auto ct = sender.second.seal(aad, pt);
  auto opened = receiver.open(aad, ct);
  if (!opened.has_value() || opened.value() != pt) {
    fails |= 8;
  }

  return fails;
}

static int
selftest_frame()
{
  // DAVE frame codec round-trip.
  uint8_t key[16];
  for (int i = 0; i < 16; i++) {
    key[i] = (uint8_t)(0x30 + i);
  }
  const char *plain = "opus-frame-payload-0123456789abcdef";
  size_t plen = std::strlen(plain);

  uint8_t wrapped[128];
  int wlen = dave_frame_encrypt(
    key, 0x11223344u, (const uint8_t *)plain, plen, wrapped, sizeof(wrapped));
  if (wlen < 0) {
    return 1;
  }

  uint32_t nonce = 0;
  if (dave_frame_peek_nonce(wrapped, (size_t)wlen, &nonce) != 0 ||
      nonce != 0x11223344u) {
    return 2;
  }

  uint8_t out[128];
  int olen =
    dave_frame_decrypt(key, wrapped, (size_t)wlen, out, sizeof(out), nullptr);
  if (olen != (int)plen || std::memcmp(out, plain, plen) != 0) {
    return 4;
  }

  // A corrupted tag must fail authentication.
  wrapped[plen] ^= 0xFF;
  if (dave_frame_decrypt(key, wrapped, (size_t)wlen, out, sizeof(out),
                         nullptr) >= 0) {
    return 8;
  }

  return 0;
}

extern "C" int
dave_selftest(void)
{
  int fails = 0;
  try {
    fails |= selftest_hpke();
    int frame = selftest_frame();
    if (frame != 0) {
      console_logf("[dave] selftest frame codec failed (0x%x)", frame);
      fails |= 32;
    }
  } catch (const std::exception& e) {
    console_logf("[dave] selftest threw: %s", e.what());
    fails |= 16;
  } catch (...) {
    fails |= 16;
  }

  if (fails == 0) {
    console_logf("[dave] crypto selftest PASS (ECDSA-P256, DHKEM-P256, HPKE, "
                 "frame codec)");
  } else {
    console_logf("[dave] crypto selftest FAIL mask=0x%x", fails);
  }
  return fails;
}


namespace {

// MLS credential identity = big-endian u64 user id.
bytes
be64(uint64_t v)
{
  std::vector<uint8_t> b(8);
  for (int i = 0; i < 8; i++) {
    b[7 - i] = (uint8_t)(v >> (8 * i));
  }
  return b;
}

// Secure Frames exporter context = little-endian u64 user id.
bytes
le64(uint64_t v)
{
  std::vector<uint8_t> b(8);
  for (int i = 0; i < 8; i++) {
    b[i] = (uint8_t)(v >> (8 * i));
  }
  return b;
}

bytes
to_bytes(const uint8_t* p, size_t n)
{
  return std::vector<uint8_t>(p, p + n);
}

uint64_t
parse_uid(const char* s)
{
  return s ? (uint64_t)std::strtoull(s, nullptr, 10) : 0;
}

}  // namespace

struct dave_session
{
  std::string user_id;
  uint64_t user_id_u64;
  uint64_t group_id;
  mlspp::CipherSuite suite;
  mlspp::SignaturePrivateKey sig_priv;
  mlspp::HPKEPrivateKey init_priv;  // KeyPackage.init_key
  mlspp::HPKEPrivateKey leaf_priv;  // LeafNode.encryption_key
  std::optional<mlspp::ExternalSender> ext_sender;
  std::optional<mlspp::KeyPackage> my_kp;
  std::optional<mlspp::State> state;

  dave_session(const std::string& uid, uint64_t gid)
    : user_id(uid)
    , user_id_u64(parse_uid(uid.c_str()))
    , group_id(gid)
    , suite(mlspp::CipherSuite::ID::P256_AES128GCM_SHA256_P256)
    , sig_priv(mlspp::SignaturePrivateKey::generate(suite))
    , init_priv(mlspp::HPKEPrivateKey::generate(suite))
    , leaf_priv(mlspp::HPKEPrivateKey::generate(suite))
  {
  }

  mlspp::LeafNode build_leaf() const
  {
    mlspp::Capabilities caps;
    caps.versions = { mlspp::ProtocolVersion::mls10 };
    caps.cipher_suites = { mlspp::CipherSuite::ID::P256_AES128GCM_SHA256_P256 };
    caps.credentials = { mlspp::CredentialType::basic };
    auto cred = mlspp::Credential::basic(be64(user_id_u64));
    return mlspp::LeafNode(suite,
                           leaf_priv.public_key,
                           sig_priv.public_key,
                           cred,
                           caps,
                           mlspp::Lifetime::create_default(),
                           mlspp::ExtensionList{},
                           sig_priv);
  }

  mlspp::ExtensionList group_extensions() const
  {
    mlspp::ExtensionList exts;
    if (ext_sender) {
      exts.add(mlspp::ExternalSendersExtension{
        { { ext_sender->signature_key, ext_sender->credential } } });
    }
    return exts;
  }
};

extern "C" dave_session*
dave_session_create(const char* self_user_id, uint64_t group_id)
{
  try {
    auto* s = new dave_session(self_user_id ? self_user_id : "", group_id);
    console_dbgf("[dave] session created (cs2 P256, user=%s)", s->user_id.c_str());
    return s;
  } catch (const std::exception& e) {
    console_logf("[dave] session create failed: %s", e.what());
    return nullptr;
  } catch (...) {
    return nullptr;
  }
}

extern "C" void
dave_session_free(dave_session* s)
{
  delete s;
}

extern "C" int
dave_established(dave_session* s)
{
  return (s && s->state) ? 1 : 0;
}

extern "C" void
dave_reset(dave_session* s)
{
  if (s) {
    s->state.reset();
    s->my_kp.reset();
  }
}

extern "C" int
dave_on_external_sender(dave_session* s, const uint8_t* pkg, size_t len)
{
  if (!s || !pkg) {
    return -1;
  }
  try {
    s->ext_sender = mlspp::tls::get<mlspp::ExternalSender>(to_bytes(pkg, len));
    return 0;
  } catch (const std::exception& e) {
    console_logf("[dave] external_sender parse: %s", e.what());
    return -1;
  } catch (...) {
    return -1;
  }
}

extern "C" int
dave_make_key_package(dave_session* s, uint8_t* out, size_t* len, size_t cap)
{
  if (!s || !out || !len) {
    return -1;
  }
  try {
    auto leaf = s->build_leaf();
    mlspp::KeyPackage kp(
      s->suite, s->init_priv.public_key, leaf, mlspp::ExtensionList{}, s->sig_priv);
    s->my_kp = kp;
    auto m = mlspp::tls::marshal(kp);
    if (m.size() > cap) {
      return -1;
    }
    std::memcpy(out, m.data(), m.size());
    *len = m.size();
    return 0;
  } catch (const std::exception& e) {
    console_logf("[dave] key_package: %s", e.what());
    return -1;
  } catch (...) {
    return -1;
  }
}

extern "C" int
dave_prepare_epoch(dave_session* s, uint64_t epoch)
{
  if (!s) {
    return -1;
  }
  if (epoch != 1) {
    return 0;  // nothing to build for epochs after 1
  }
  if (!s->ext_sender) {
    return -1;
  }
  try {
    auto leaf = s->build_leaf();
    mlspp::State st(be64(s->group_id),
                    s->suite,
                    s->leaf_priv,
                    s->sig_priv,
                    leaf,
                    s->group_extensions());
    s->state = std::move(st);
    console_dbgf("[dave] created MLS group (epoch 1)");
    return 0;
  } catch (const std::exception& e) {
    console_logf("[dave] create group: %s", e.what());
    return -1;
  } catch (...) {
    return -1;
  }
}

extern "C" int
dave_on_welcome(dave_session* s, const uint8_t* data, size_t len,
                const char* const* recognized, int nrecognized)
{
  if (!s || !data || !s->my_kp) {
    return -1;
  }
  (void)recognized;
  (void)nrecognized;  // recognized[] is not validated
  try {
    auto welcome = mlspp::tls::get<mlspp::Welcome>(to_bytes(data, len));
    mlspp::State st(s->init_priv,
                    s->leaf_priv,
                    s->sig_priv,
                    *s->my_kp,
                    welcome,
                    std::nullopt,  // ratchet tree is inline in the Welcome
                    {});  // no PSKs
    s->state = std::move(st);
    console_dbgf("[dave] joined MLS group via welcome, epoch=%llu",
                 (unsigned long long)s->state->epoch());
    return 0;
  } catch (const std::exception& e) {
    console_logf("[dave] welcome: %s", e.what());
    return -1;
  } catch (...) {
    return -1;
  }
}

extern "C" int
dave_on_commit(dave_session* s, const uint8_t* data, size_t len)
{
  if (!s || !s->state || !data) {
    return -1;
  }
  try {
    auto msg = mlspp::tls::get<mlspp::MLSMessage>(to_bytes(data, len));
    auto next = s->state->handle(msg);
    if (!next) {
      return -1;  // our own commit echoed back, or not applicable
    }
    s->state = std::move(*next);
    console_dbgf("[dave] processed commit, epoch=%llu",
                 (unsigned long long)s->state->epoch());
    return 0;
  } catch (const std::exception& e) {
    console_logf("[dave] commit: %s", e.what());
    return -1;
  } catch (...) {
    return -1;
  }
}

extern "C" int
dave_on_proposals(dave_session* s, const uint8_t* data, size_t len,
                  const char* const* recognized, int nrecognized, uint8_t* out,
                  size_t* out_len, size_t cap, uint8_t* wout, size_t* wout_len,
                  size_t wcap)
{
  if (!s || !s->state || !data || !out || !out_len || !wout || !wout_len) {
    return -1;
  }
  (void)recognized;
  (void)nrecognized;  // recognized[] is not validated
  try {
    // Proposals from the external sender: a TLS list of MLSMessage. Cache each
    // via handle(), then produce a Commit (all v1.0 members are committers).
    auto msgs =
      mlspp::tls::get<std::vector<mlspp::MLSMessage>>(to_bytes(data, len));
    for (auto& m : msgs) {
      s->state->handle(m);  // caches the proposal (returns nullopt)
    }
    // inline_tree=true so the emitted Welcome carries the ratchet tree; the join
    // path (dave_on_welcome) reads it with tree=nullopt, so both must agree.
    mlspp::CommitOpts opts;
    opts.inline_tree = true;
    auto [commit, welcome, next] =
      s->state->commit(mlspp::hpke::random_bytes(32), opts, {});
    // Marshal and bounds-check both Commit and Welcome before advancing the epoch,
    // so a too-small buffer leaves MLS state untouched.
    auto mc = mlspp::tls::marshal(commit);
    auto mw = mlspp::tls::marshal(welcome);
    if (mc.size() > cap || mw.size() > wcap) {
      return -1;
    }
    s->state = std::move(next);
    std::memcpy(out, mc.data(), mc.size());
    *out_len = mc.size();
    std::memcpy(wout, mw.data(), mw.size());
    *wout_len = mw.size();
    return 0;
  } catch (const std::exception& e) {
    console_logf("[dave] proposals: %s", e.what());
    return -1;
  } catch (...) {
    return -1;
  }
}

extern "C" int
dave_sender_key(dave_session* s, const char* sender_user_id, uint8_t generation,
                uint8_t out_key[16])
{
  if (!s || !s->state || !sender_user_id) {
    return -1;
  }
  try {
    uint64_t uid = parse_uid(sender_user_id);
    // base = MLS-Exporter("Discord Secure Frames v0", LE64(user_id), 16); the
    // exporter hashes the context internally, so pass the raw 8 bytes.
    auto base = s->state->do_export("Discord Secure Frames v0", le64(uid), 16);
    mlspp::HashRatchet ratchet(s->suite, base);
    auto kn = ratchet.get(generation);
    if (kn.key.size() < 16) {
      return -1;
    }
    std::memcpy(out_key, kn.key.data(), 16);
    return 0;
  } catch (const std::exception& e) {
    console_logf("[dave] sender_key: %s", e.what());
    return -1;
  } catch (...) {
    return -1;
  }
}
