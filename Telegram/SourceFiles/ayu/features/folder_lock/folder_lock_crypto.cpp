// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/folder_lock/folder_lock_crypto.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <memory>

namespace Ayu::FolderLockCrypto {
namespace {

constexpr auto kScryptMaxMem = uint64_t(256) * 1024 * 1024;
constexpr auto kVersion = char(1);
constexpr char kAadPrefix[] = "ayu-folder-lock-v1";

struct CtxDeleter {
	void operator()(EVP_CIPHER_CTX *ctx) const {
		EVP_CIPHER_CTX_free(ctx);
	}
};
using CtxPointer = std::unique_ptr<EVP_CIPHER_CTX, CtxDeleter>;

[[nodiscard]] const unsigned char *U(const QByteArray &data) {
	return reinterpret_cast<const unsigned char*>(data.constData());
}

[[nodiscard]] unsigned char *U(QByteArray &data) {
	return reinterpret_cast<unsigned char*>(data.data());
}

} // namespace

QByteArray RandomBytes(int size) {
	if (size <= 0) {
		return {};
	}
	auto result = QByteArray(size, Qt::Uninitialized);
	if (RAND_bytes(U(result), size) != 1) {
		return {};
	}
	return result;
}

QByteArray MakeAad(int filterId) {
	auto result = QByteArray(kAadPrefix);
	for (auto i = 0; i != 4; ++i) {
		result.append(char((uint32_t(filterId) >> (8 * i)) & 0xFF));
	}
	return result;
}

QByteArray DeriveKey(
		const QByteArray &pin,
		const QByteArray &salt,
		int slot,
		const KdfParams &params) {
	auto slotSalt = salt;
	slotSalt.append(char(slot));
	auto result = QByteArray(kKeySize, Qt::Uninitialized);
	const auto ok = EVP_PBE_scrypt(
		pin.constData(),
		size_t(pin.size()),
		U(slotSalt),
		size_t(slotSalt.size()),
		params.n,
		params.r,
		params.p,
		kScryptMaxMem,
		U(result),
		size_t(result.size()));
	if (ok != 1) {
		Wipe(result);
		return {};
	}
	return result;
}

QByteArray SealSlot(
		const QByteArray &key,
		const QByteArray &aad,
		const SlotContent &content) {
	if (key.size() != kKeySize || content.data.size() > kMaxDataSize) {
		return {};
	}
	auto payload = QByteArray();
	payload.reserve(kPayloadSize);
	payload.append(kVersion);
	payload.append(char(content.role));
	payload.append(char((content.data.size() >> 8) & 0xFF));
	payload.append(char(content.data.size() & 0xFF));
	payload.append(content.data);
	const auto paddingSize = kPayloadSize - int(payload.size());
	if (paddingSize > 0) {
		const auto padding = RandomBytes(paddingSize);
		if (padding.size() != paddingSize) {
			Wipe(payload);
			return {};
		}
		payload.append(padding);
	}

	const auto nonce = RandomBytes(kNonceSize);
	if (nonce.size() != kNonceSize) {
		Wipe(payload);
		return {};
	}
	auto cipher = QByteArray(kPayloadSize, Qt::Uninitialized);
	auto tag = QByteArray(kTagSize, Qt::Uninitialized);
	const auto ctx = CtxPointer(EVP_CIPHER_CTX_new());
	auto len = 0;
	auto finalLen = 0;
	const auto ok = ctx
		&& (EVP_EncryptInit_ex(
			ctx.get(),
			EVP_aes_256_gcm(),
			nullptr,
			nullptr,
			nullptr) == 1)
		&& (EVP_CIPHER_CTX_ctrl(
			ctx.get(),
			EVP_CTRL_GCM_SET_IVLEN,
			kNonceSize,
			nullptr) == 1)
		&& (EVP_EncryptInit_ex(
			ctx.get(),
			nullptr,
			nullptr,
			U(key),
			U(nonce)) == 1)
		&& (EVP_EncryptUpdate(
			ctx.get(),
			nullptr,
			&len,
			U(aad),
			int(aad.size())) == 1)
		&& (EVP_EncryptUpdate(
			ctx.get(),
			U(cipher),
			&len,
			U(payload),
			int(payload.size())) == 1)
		&& (len == kPayloadSize)
		&& (EVP_EncryptFinal_ex(ctx.get(), U(cipher) + len, &finalLen) == 1)
		&& (finalLen == 0)
		&& (EVP_CIPHER_CTX_ctrl(
			ctx.get(),
			EVP_CTRL_GCM_GET_TAG,
			kTagSize,
			U(tag)) == 1);
	Wipe(payload);
	if (!ok) {
		return {};
	}
	return nonce + cipher + tag;
}

std::optional<SlotContent> OpenSlot(
		const QByteArray &key,
		const QByteArray &aad,
		const QByteArray &slot) {
	if (key.size() != kKeySize || slot.size() != kSlotSize) {
		return std::nullopt;
	}
	const auto nonce = slot.mid(0, kNonceSize);
	const auto cipher = slot.mid(kNonceSize, kPayloadSize);
	auto tag = slot.mid(kNonceSize + kPayloadSize, kTagSize);
	auto payload = QByteArray(kPayloadSize, Qt::Uninitialized);
	const auto ctx = CtxPointer(EVP_CIPHER_CTX_new());
	auto len = 0;
	auto finalLen = 0;
	const auto ok = ctx
		&& (EVP_DecryptInit_ex(
			ctx.get(),
			EVP_aes_256_gcm(),
			nullptr,
			nullptr,
			nullptr) == 1)
		&& (EVP_CIPHER_CTX_ctrl(
			ctx.get(),
			EVP_CTRL_GCM_SET_IVLEN,
			kNonceSize,
			nullptr) == 1)
		&& (EVP_DecryptInit_ex(
			ctx.get(),
			nullptr,
			nullptr,
			U(key),
			U(nonce)) == 1)
		&& (EVP_DecryptUpdate(
			ctx.get(),
			nullptr,
			&len,
			U(aad),
			int(aad.size())) == 1)
		&& (EVP_DecryptUpdate(
			ctx.get(),
			U(payload),
			&len,
			U(cipher),
			int(cipher.size())) == 1)
		&& (len == kPayloadSize)
		&& (EVP_CIPHER_CTX_ctrl(
			ctx.get(),
			EVP_CTRL_GCM_SET_TAG,
			kTagSize,
			U(tag)) == 1)
		&& (EVP_DecryptFinal_ex(ctx.get(), U(payload) + len, &finalLen) == 1);
	if (!ok || payload[0] != kVersion) {
		Wipe(payload);
		return std::nullopt;
	}
	const auto role = static_cast<Role>(
		static_cast<unsigned char>(payload[1]));
	const auto size = (int(static_cast<unsigned char>(payload[2])) << 8)
		| int(static_cast<unsigned char>(payload[3]));
	if ((role != Role::Full && role != Role::Decoy) || size > kMaxDataSize) {
		Wipe(payload);
		return std::nullopt;
	}
	auto result = SlotContent{
		.role = role,
		.data = payload.mid(kPayloadHeaderSize, size),
	};
	Wipe(payload);
	return result;
}

CheckResult CheckPin(
		const QByteArray &pin,
		const QByteArray &salt,
		const KdfParams &params,
		int filterId,
		const std::array<QByteArray, 2> &slots) {
	const auto aad = MakeAad(filterId);
	auto result = CheckResult();
	for (auto i = 0; i != int(slots.size()); ++i) {
		auto key = DeriveKey(pin, salt, i, params);
		auto opened = OpenSlot(key, aad, slots[i]);
		Wipe(key);
		if (opened && !result.ok) {
			result = CheckResult{
				.ok = true,
				.slot = i,
				.content = std::move(*opened),
			};
		}
	}
	return result;
}

int RetryDelaySeconds(int badTries) {
	if (badTries <= 3) {
		return 0;
	}
	switch (badTries) {
	case 4: return 5;
	case 5: return 10;
	case 6: return 20;
	case 7: return 40;
	}
	return 60;
}

void Wipe(QByteArray &data) {
	if (!data.isEmpty()) {
		OPENSSL_cleanse(data.data(), size_t(data.size()));
	}
	data.clear();
}

} // namespace Ayu::FolderLockCrypto
