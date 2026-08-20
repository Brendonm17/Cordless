#include <hpke/base64.h>

#include <algorithm>
#include <stdexcept>

namespace MLS_NAMESPACE::hpke {

static const char*
b64_alphabet()
{
  return "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

static int
b64_value(char c)
{
  const auto* a = b64_alphabet();
  for (int i = 0; i < 64; i++) {
    if (a[i] == c) {
      return i;
    }
  }
  return -1;
}

std::string
to_base64(const bytes& data)
{
  if (data.empty()) {
    return "";
  }

  const auto* a = b64_alphabet();
  std::string out;
  out.reserve((data.size() + 2) / 3 * 4);

  size_t i = 0;
  const auto n = data.size();
  while (i + 3 <= n) {
    const uint32_t v = (uint32_t(data.at(i)) << 16) |
                       (uint32_t(data.at(i + 1)) << 8) | uint32_t(data.at(i + 2));
    out.push_back(a[(v >> 18) & 0x3f]);
    out.push_back(a[(v >> 12) & 0x3f]);
    out.push_back(a[(v >> 6) & 0x3f]);
    out.push_back(a[v & 0x3f]);
    i += 3;
  }

  const auto rem = n - i;
  if (rem == 1) {
    const uint32_t v = uint32_t(data.at(i)) << 16;
    out.push_back(a[(v >> 18) & 0x3f]);
    out.push_back(a[(v >> 12) & 0x3f]);
    out.push_back('=');
    out.push_back('=');
  } else if (rem == 2) {
    const uint32_t v =
      (uint32_t(data.at(i)) << 16) | (uint32_t(data.at(i + 1)) << 8);
    out.push_back(a[(v >> 18) & 0x3f]);
    out.push_back(a[(v >> 12) & 0x3f]);
    out.push_back(a[(v >> 6) & 0x3f]);
    out.push_back('=');
  }

  return out;
}

std::string
to_base64url(const bytes& data)
{
  if (data.empty()) {
    return "";
  }

  auto encoded = to_base64(data);

  auto pad_start = encoded.find_first_of('=');
  if (pad_start != std::string::npos) {
    encoded = encoded.substr(0, pad_start);
  }

  std::replace(encoded.begin(), encoded.end(), '+', '-');
  std::replace(encoded.begin(), encoded.end(), '/', '_');

  return encoded;
}

bytes
from_base64(const std::string& enc)
{
  if (enc.empty()) {
    return {};
  }

  if (enc.length() % 4 != 0) {
    throw std::runtime_error("Base64 length is not divisible by 4");
  }

  size_t pad = 0;
  if (enc[enc.length() - 1] == '=') {
    pad++;
  }
  if (enc[enc.length() - 2] == '=') {
    pad++;
  }

  bytes out;
  out.reserve(enc.length() / 4 * 3);

  for (size_t i = 0; i < enc.length(); i += 4) {
    int v0 = b64_value(enc[i]);
    int v1 = b64_value(enc[i + 1]);
    int v2 = (enc[i + 2] == '=') ? 0 : b64_value(enc[i + 2]);
    int v3 = (enc[i + 3] == '=') ? 0 : b64_value(enc[i + 3]);
    if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0) {
      throw std::runtime_error("Invalid base64 character");
    }

    const uint32_t v = (uint32_t(v0) << 18) | (uint32_t(v1) << 12) |
                       (uint32_t(v2) << 6) | uint32_t(v3);
    out.push_back(uint8_t((v >> 16) & 0xff));
    out.push_back(uint8_t((v >> 8) & 0xff));
    out.push_back(uint8_t(v & 0xff));
  }

  if (pad > 0) {
    out.resize(out.size() - pad);
  }

  return out;
}

bytes
from_base64url(const std::string& enc)
{
  if (enc.empty()) {
    return {};
  }

  auto enc_copy = enc;
  std::replace(enc_copy.begin(), enc_copy.end(), '-', '+');
  std::replace(enc_copy.begin(), enc_copy.end(), '_', '/');

  while (enc_copy.length() % 4 != 0) {
    enc_copy += "=";
  }

  return from_base64(enc_copy);
}

} // namespace MLS_NAMESPACE::hpke
