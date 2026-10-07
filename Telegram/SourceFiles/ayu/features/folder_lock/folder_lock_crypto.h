// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include <QtCore/QByteArray>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace Ayu::FolderLockCrypto {

inline constexpr auto kSlotSize = 4096;
inline constexpr auto kSaltSize = 32;
inline constexpr auto kKeySize = 32;
inline constexpr auto kNonceSize = 12;
inline constexpr auto kTagSize = 16;
inline constexpr auto kPayloadSize = kSlotSize - kNonceSize - kTagSize;
inline constexpr auto kPayloadHeaderSize = 4;
inline constexpr auto kMaxDataSize = kPayloadSize - kPayloadHeaderSize;
inline constexpr auto kMinPinLength = 6;

struct KdfParams {
	uint64_t n = (uint64_t(1) << 17);
	uint64_t r = 8;
	uint64_t p = 1;
};

enum class Role : unsigned char {
	Full = 1,
	Decoy = 2,
};

struct SlotContent {
	Role role = Role::Full;
	QByteArray data;
};

struct CheckResult {
	bool ok = false;
	bool failed = false; // Key derivation failed (broken record), not a PIN.
	int slot = -1;
	SlotContent content;
	QByteArray key; // Key of the matched slot (to re-seal it later).
};

// Data of the real slot (0): the key of the decoy slot (1) if a second
// bottom is set up, and the list of allowed chats.
struct SecretData {
	QByteArray decoyKey;
	std::vector<uint64_t> allowed;
};

// Fits into one slot together with SecretData header and the decoy key.
inline constexpr auto kMaxAllowed = (kMaxDataSize - 2 - kKeySize - 3) / 8;

[[nodiscard]] QByteArray RandomBytes(int size);
[[nodiscard]] QByteArray MakeAad(int filterId);
[[nodiscard]] QByteArray DeriveKey(
	const QByteArray &pin,
	const QByteArray &salt,
	int slot,
	const KdfParams &params);
[[nodiscard]] QByteArray SealSlot(
	const QByteArray &key,
	const QByteArray &aad,
	const SlotContent &content);
[[nodiscard]] std::optional<SlotContent> OpenSlot(
	const QByteArray &key,
	const QByteArray &aad,
	const QByteArray &slot);
// Always derives keys for both slots so timing doesn't tell which matched.
[[nodiscard]] CheckResult CheckPin(
	const QByteArray &pin,
	const QByteArray &salt,
	const KdfParams &params,
	int filterId,
	const std::array<QByteArray, 2> &slots);
[[nodiscard]] QByteArray SerializeAllowed(const std::vector<uint64_t> &ids);
[[nodiscard]] std::optional<std::vector<uint64_t>> ParseAllowed(
	const QByteArray &data);
[[nodiscard]] QByteArray SerializeSecret(const SecretData &data);
// Empty data (records of sub-project 1) is a SecretData without a decoy.
[[nodiscard]] std::optional<SecretData> ParseSecret(const QByteArray &data);
// Seconds to wait after `badTries` wrong PINs in a row.
[[nodiscard]] int RetryDelaySeconds(int badTries);
// Best effort: zeroes the buffer if `data` is its only owner (other
// QByteArray copies sharing it, and Qt's own input field text, stay).
void Wipe(QByteArray &data);

} // namespace Ayu::FolderLockCrypto
