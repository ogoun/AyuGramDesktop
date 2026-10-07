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
#include <openssl/sha.h>

#include <memory>

namespace Ayu::FolderLockCrypto {
namespace {

constexpr auto kScryptMaxMem = uint64_t(256) * 1024 * 1024;
constexpr auto kVersion = char(1);
constexpr char kAadPrefix[] = "ayu-folder-lock-v1";
constexpr char kBoxContext[] = "ayu-folder-box-v1";
constexpr char kTagContext[] = "ayu-folder-tag-v1";
constexpr auto kBoxVersion = char(1);
constexpr auto kBoxHeaderSize = 1 + kBoxKeySize + kNonceSize;
constexpr auto kSecretVersion = char(2);

struct CtxDeleter {
	void operator()(EVP_CIPHER_CTX *ctx) const {
		EVP_CIPHER_CTX_free(ctx);
	}
};
using CtxPointer = std::unique_ptr<EVP_CIPHER_CTX, CtxDeleter>;

struct PkeyDeleter {
	void operator()(EVP_PKEY *key) const {
		EVP_PKEY_free(key);
	}
};
using PkeyPointer = std::unique_ptr<EVP_PKEY, PkeyDeleter>;

struct PkeyCtxDeleter {
	void operator()(EVP_PKEY_CTX *ctx) const {
		EVP_PKEY_CTX_free(ctx);
	}
};
using PkeyCtxPointer = std::unique_ptr<EVP_PKEY_CTX, PkeyCtxDeleter>;

[[nodiscard]] const unsigned char *U(const QByteArray &data) {
	return reinterpret_cast<const unsigned char*>(data.constData());
}

[[nodiscard]] unsigned char *U(QByteArray &data) {
	return reinterpret_cast<unsigned char*>(data.data());
}

[[nodiscard]] QByteArray Sha256(const QByteArray &data) {
	auto result = QByteArray(SHA256_DIGEST_LENGTH, Qt::Uninitialized);
	SHA256(U(data), size_t(data.size()), U(result));
	return result;
}

[[nodiscard]] PkeyPointer PrivateKey(const QByteArray &key) {
	if (key.size() != kBoxKeySize) {
		return nullptr;
	}
	return PkeyPointer(EVP_PKEY_new_raw_private_key(
		EVP_PKEY_X25519,
		nullptr,
		U(key),
		size_t(key.size())));
}

[[nodiscard]] PkeyPointer PublicKey(const QByteArray &key) {
	if (key.size() != kBoxKeySize) {
		return nullptr;
	}
	return PkeyPointer(EVP_PKEY_new_raw_public_key(
		EVP_PKEY_X25519,
		nullptr,
		U(key),
		size_t(key.size())));
}

[[nodiscard]] QByteArray RawPublic(EVP_PKEY *key) {
	auto result = QByteArray(kBoxKeySize, Qt::Uninitialized);
	auto length = size_t(result.size());
	if (!key
		|| EVP_PKEY_get_raw_public_key(key, U(result), &length) != 1
		|| length != size_t(kBoxKeySize)) {
		return {};
	}
	return result;
}

// The box key: SHA-256 of the context, the shared secret and both keys.
[[nodiscard]] QByteArray BoxKey(
		EVP_PKEY *own,
		EVP_PKEY *peer,
		const QByteArray &ephemeralPublic,
		const QByteArray &recipientPublic) {
	const auto ctx = PkeyCtxPointer(EVP_PKEY_CTX_new(own, nullptr));
	auto shared = QByteArray(kBoxKeySize, Qt::Uninitialized);
	auto length = size_t(shared.size());
	const auto ok = ctx
		&& (EVP_PKEY_derive_init(ctx.get()) == 1)
		&& (EVP_PKEY_derive_set_peer(ctx.get(), peer) == 1)
		&& (EVP_PKEY_derive(ctx.get(), U(shared), &length) == 1)
		&& (length == size_t(kBoxKeySize));
	if (!ok) {
		Wipe(shared);
		return {};
	}
	auto material = QByteArray(kBoxContext)
		+ shared
		+ ephemeralPublic
		+ recipientPublic;
	auto result = Sha256(material);
	Wipe(material);
	Wipe(shared);
	return result;
}

// AES-256-GCM of a whole buffer, `cipher` and `tag` are set on success.
[[nodiscard]] bool Encrypt(
		const QByteArray &key,
		const QByteArray &nonce,
		const QByteArray &aad,
		const QByteArray &plain,
		QByteArray &cipher,
		QByteArray &tag) {
	cipher = QByteArray(plain.size(), Qt::Uninitialized);
	tag = QByteArray(kTagSize, Qt::Uninitialized);
	const auto ctx = CtxPointer(EVP_CIPHER_CTX_new());
	auto len = 0;
	auto finalLen = 0;
	return ctx
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
			U(plain),
			int(plain.size())) == 1)
		&& (len == plain.size())
		&& (EVP_EncryptFinal_ex(ctx.get(), U(cipher) + len, &finalLen) == 1)
		&& (finalLen == 0)
		&& (EVP_CIPHER_CTX_ctrl(
			ctx.get(),
			EVP_CTRL_GCM_GET_TAG,
			kTagSize,
			U(tag)) == 1);
}

[[nodiscard]] bool Decrypt(
		const QByteArray &key,
		const QByteArray &nonce,
		const QByteArray &aad,
		const QByteArray &cipher,
		QByteArray tag,
		QByteArray &plain) {
	plain = QByteArray(cipher.size(), Qt::Uninitialized);
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
			U(plain),
			&len,
			U(cipher),
			int(cipher.size())) == 1)
		&& (len == cipher.size())
		&& (EVP_CIPHER_CTX_ctrl(
			ctx.get(),
			EVP_CTRL_GCM_SET_TAG,
			kTagSize,
			U(tag)) == 1)
		&& (EVP_DecryptFinal_ex(ctx.get(), U(plain) + len, &finalLen) == 1)
		&& (finalLen == 0);
	if (!ok) {
		Wipe(plain);
	}
	return ok;
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
		if (key.isEmpty()) {
			result.failed = true;
		}
		auto opened = OpenSlot(key, aad, slots[i]);
		if (opened && !result.ok) {
			result.ok = true;
			result.slot = i;
			result.content = std::move(*opened);
			result.key = key;
		}
		Wipe(key);
	}
	return result;
}

QByteArray SerializeAllowed(const std::vector<uint64_t> &ids) {
	auto result = QByteArray();
	result.reserve(3 + 8 * int(ids.size()));
	result.append(char(1));
	result.append(char((ids.size() >> 8) & 0xFF));
	result.append(char(ids.size() & 0xFF));
	for (const auto id : ids) {
		for (auto i = 0; i != 8; ++i) {
			result.append(char((id >> (8 * i)) & 0xFF));
		}
	}
	return result;
}

std::optional<std::vector<uint64_t>> ParseAllowed(const QByteArray &data) {
	if (data.isEmpty()) {
		return std::vector<uint64_t>();
	} else if (data.size() < 3 || data[0] != char(1)) {
		return std::nullopt;
	}
	const auto count = (int(uchar(data[1])) << 8) | int(uchar(data[2]));
	if (data.size() != 3 + 8 * count) {
		return std::nullopt;
	}
	auto result = std::vector<uint64_t>();
	result.reserve(count);
	for (auto k = 0; k != count; ++k) {
		auto id = uint64_t();
		for (auto i = 0; i != 8; ++i) {
			id |= uint64_t(uchar(data[3 + 8 * k + i])) << (8 * i);
		}
		result.push_back(id);
	}
	return result;
}

// Version 2: 2 | decoy key length | decoy key | folder key length |
// folder key | allowed. Version 1 (sub-project 2) has no folder key.
QByteArray SerializeSecret(const SecretData &data) {
	auto result = QByteArray();
	result.append(kSecretVersion);
	result.append(char(data.decoyKey.size()));
	result.append(data.decoyKey);
	result.append(char(data.folderKey.size()));
	result.append(data.folderKey);
	result.append(SerializeAllowed(data.allowed));
	return result;
}

std::optional<SecretData> ParseSecret(const QByteArray &data) {
	if (data.isEmpty()) {
		return SecretData(); // Records of sub-project 1: no second bottom.
	} else if (data.size() < 2
		|| (data[0] != char(1) && data[0] != kSecretVersion)) {
		return std::nullopt;
	}
	const auto version = data[0];
	auto offset = 1;
	const auto readKey = [&](int expected) -> std::optional<QByteArray> {
		if (data.size() < offset + 1) {
			return std::nullopt;
		}
		const auto length = int(uchar(data[offset]));
		if ((length != 0 && length != expected)
			|| data.size() < offset + 1 + length) {
			return std::nullopt;
		}
		auto result = data.mid(offset + 1, length);
		offset += 1 + length;
		return result;
	};
	const auto decoyKey = readKey(kKeySize);
	if (!decoyKey) {
		return std::nullopt;
	}
	auto folderKey = QByteArray();
	if (version == kSecretVersion) {
		const auto key = readKey(kBoxKeySize);
		if (!key) {
			return std::nullopt;
		}
		folderKey = *key;
	}
	auto allowed = ParseAllowed(data.mid(offset));
	if (!allowed) {
		return std::nullopt;
	}
	return SecretData{
		.decoyKey = *decoyKey,
		.allowed = std::move(*allowed),
		.folderKey = std::move(folderKey),
	};
}

KeyPair GenerateKeyPair() {
	const auto ctx = PkeyCtxPointer(
		EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr));
	auto raw = static_cast<EVP_PKEY*>(nullptr);
	if (!ctx
		|| EVP_PKEY_keygen_init(ctx.get()) != 1
		|| EVP_PKEY_keygen(ctx.get(), &raw) != 1) {
		return {};
	}
	const auto key = PkeyPointer(raw);
	auto result = KeyPair{
		.publicKey = RawPublic(key.get()),
		.privateKey = QByteArray(kBoxKeySize, Qt::Uninitialized),
	};
	auto length = size_t(result.privateKey.size());
	if (result.publicKey.isEmpty()
		|| EVP_PKEY_get_raw_private_key(
			key.get(),
			U(result.privateKey),
			&length) != 1
		|| length != size_t(kBoxKeySize)) {
		Wipe(result.privateKey);
		return {};
	}
	return result;
}

QByteArray PublicKeyOf(const QByteArray &privateKey) {
	const auto key = PrivateKey(privateKey);
	return key ? RawPublic(key.get()) : QByteArray();
}

// Box: version | ephemeral public key | nonce | cipher | tag. The plain
// text inside: data length (4 bytes BE) | data | random padding.
QByteArray SealBox(const QByteArray &publicKey, const QByteArray &data) {
	const auto recipient = PublicKey(publicKey);
	if (!recipient) {
		return {};
	}
	auto ephemeral = GenerateKeyPair();
	const auto own = PrivateKey(ephemeral.privateKey);
	Wipe(ephemeral.privateKey);
	if (!own || ephemeral.publicKey.isEmpty()) {
		return {};
	}
	auto key = BoxKey(
		own.get(),
		recipient.get(),
		ephemeral.publicKey,
		publicKey);
	if (key.size() != kKeySize) {
		return {};
	}
	const auto contentSize = 4 + int(data.size());
	const auto paddedSize = ((contentSize + kBoxPadding - 1) / kBoxPadding)
		* kBoxPadding;
	auto plain = QByteArray();
	plain.reserve(paddedSize);
	const auto size = uint32_t(data.size());
	for (auto i = 3; i >= 0; --i) {
		plain.append(char((size >> (8 * i)) & 0xFF));
	}
	plain.append(data);
	if (paddedSize > contentSize) {
		plain.append(RandomBytes(paddedSize - contentSize));
	}
	const auto nonce = RandomBytes(kNonceSize);
	auto header = QByteArray();
	header.append(kBoxVersion);
	header.append(ephemeral.publicKey);
	auto cipher = QByteArray();
	auto tag = QByteArray();
	const auto ok = (plain.size() == paddedSize)
		&& (nonce.size() == kNonceSize)
		&& Encrypt(key, nonce, header, plain, cipher, tag);
	Wipe(plain);
	Wipe(key);
	if (!ok) {
		return {};
	}
	return header + nonce + cipher + tag;
}

std::optional<QByteArray> OpenBox(
		const QByteArray &privateKey,
		const QByteArray &box) {
	if (box.size() < kBoxHeaderSize + kBoxPadding + kTagSize
		|| box[0] != kBoxVersion
		|| (box.size() - kBoxHeaderSize - kTagSize) % kBoxPadding) {
		return std::nullopt;
	}
	const auto own = PrivateKey(privateKey);
	const auto ephemeralPublic = box.mid(1, kBoxKeySize);
	const auto ephemeral = PublicKey(ephemeralPublic);
	const auto recipientPublic = PublicKeyOf(privateKey);
	if (!own || !ephemeral || recipientPublic.isEmpty()) {
		return std::nullopt;
	}
	auto key = BoxKey(
		own.get(),
		ephemeral.get(),
		ephemeralPublic,
		recipientPublic);
	if (key.size() != kKeySize) {
		return std::nullopt;
	}
	const auto header = box.left(1 + kBoxKeySize);
	const auto nonce = box.mid(1 + kBoxKeySize, kNonceSize);
	const auto cipher = box.mid(
		kBoxHeaderSize,
		box.size() - kBoxHeaderSize - kTagSize);
	const auto tag = box.right(kTagSize);
	auto plain = QByteArray();
	const auto ok = Decrypt(key, nonce, header, cipher, tag, plain);
	Wipe(key);
	if (!ok || plain.size() < 4) {
		Wipe(plain);
		return std::nullopt;
	}
	auto size = uint32_t();
	for (auto i = 0; i != 4; ++i) {
		size = (size << 8) | uint32_t(uchar(plain[i]));
	}
	if (size > uint32_t(plain.size() - 4)) {
		Wipe(plain);
		return std::nullopt;
	}
	auto result = plain.mid(4, int(size));
	Wipe(plain);
	return result;
}

QByteArray KeyTag(const QByteArray &publicKey) {
	return Sha256(QByteArray(kTagContext) + publicKey).left(kKeyTagSize);
}

int RetryDelaySeconds(int badTries) {
	if (badTries < 3) {
		return 0;
	}
	switch (badTries) {
	case 3: return 5;
	case 4: return 10;
	case 5: return 20;
	case 6: return 40;
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
