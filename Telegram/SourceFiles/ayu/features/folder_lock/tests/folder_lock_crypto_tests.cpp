// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
//
// Unit tests of the folder lock crypto core (target test_ayu_folder_lock).
#include "ayu/features/folder_lock/folder_lock_crypto.h"
#include "ayu/features/folder_lock/folder_lock_merge.h"

#include <QtCore/QString>

#include <cstdio>

namespace {

int Failed = 0;

#define AYU_CHECK(condition) do { \
	if (!(condition)) { \
		std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
		++Failed; \
	} \
} while (false)

using namespace Ayu::FolderLockCrypto;

// Fast parameters: the real N = 2^17 is too slow for a test run.
const auto kFast = KdfParams{ .n = (uint64_t(1) << 10), .r = 8, .p = 1 };

void TestDeriveKey() {
	const auto salt = QByteArray(kSaltSize, 'S');
	const auto a = DeriveKey("secret1", salt, 0, kFast);
	const auto b = DeriveKey("secret1", salt, 0, kFast);
	const auto c = DeriveKey("secret1", salt, 1, kFast);
	const auto d = DeriveKey("secret2", salt, 0, kFast);
	AYU_CHECK(a.size() == kKeySize);
	AYU_CHECK(a == b);
	AYU_CHECK(a != c);
	AYU_CHECK(a != d);
}

void TestSealOpen() {
	const auto key = RandomBytes(kKeySize);
	const auto aad = MakeAad(7);
	const auto content = SlotContent{ .role = Role::Decoy, .data = "chats" };
	const auto slot = SealSlot(key, aad, content);
	AYU_CHECK(slot.size() == kSlotSize);
	const auto opened = OpenSlot(key, aad, slot);
	AYU_CHECK(opened.has_value());
	AYU_CHECK(opened && opened->role == Role::Decoy);
	AYU_CHECK(opened && opened->data == "chats");
	AYU_CHECK(!OpenSlot(RandomBytes(kKeySize), aad, slot).has_value());
	AYU_CHECK(!OpenSlot(key, MakeAad(8), slot).has_value());
	auto tampered = slot;
	if (tampered.size() > 100) {
		tampered[100] = char(tampered[100] ^ 0x01);
	}
	AYU_CHECK(!OpenSlot(key, aad, tampered).has_value());
	AYU_CHECK(!OpenSlot(key, aad, slot.left(kSlotSize - 1)).has_value());
	const auto big = SlotContent{ .data = QByteArray(kMaxDataSize + 1, 'x') };
	AYU_CHECK(SealSlot(key, aad, big).isEmpty());
	const auto max = SlotContent{ .data = QByteArray(kMaxDataSize, 'x') };
	AYU_CHECK(SealSlot(key, aad, max).size() == kSlotSize);
	const auto two = SealSlot(key, aad, content);
	AYU_CHECK(two.size() == kSlotSize && two != slot); // Random nonce.
}

void TestCheckPin() {
	const auto salt = RandomBytes(kSaltSize);
	const auto filterId = 5;
	const auto aad = MakeAad(filterId);
	const auto key0 = DeriveKey("first-pin", salt, 0, kFast);
	const auto key1 = DeriveKey("second-pin", salt, 1, kFast);
	const auto slots = std::array<QByteArray, 2>{
		SealSlot(key0, aad, SlotContent{ .role = Role::Full }),
		SealSlot(key1, aad, SlotContent{ .role = Role::Decoy }),
	};
	const auto r0 = CheckPin("first-pin", salt, kFast, filterId, slots);
	AYU_CHECK(r0.ok && r0.slot == 0 && r0.content.role == Role::Full);
	const auto r1 = CheckPin("second-pin", salt, kFast, filterId, slots);
	AYU_CHECK(r1.ok && r1.slot == 1 && r1.content.role == Role::Decoy);
	const auto bad = CheckPin("wrong-pin", salt, kFast, filterId, slots);
	AYU_CHECK(!bad.ok && bad.slot == -1);
	const auto otherFilter = CheckPin("first-pin", salt, kFast, 6, slots);
	AYU_CHECK(!otherFilter.ok);

	const auto random = std::array<QByteArray, 2>{
		SealSlot(key0, aad, SlotContent{ .role = Role::Full }),
		RandomBytes(kSlotSize),
	};
	AYU_CHECK(CheckPin("first-pin", salt, kFast, filterId, random).ok);
	AYU_CHECK(!CheckPin("second-pin", salt, kFast, filterId, random).ok);

	// Unicode PIN with letters and symbols.
	const auto unicode = QString::fromUtf8("Пароль-№1!").toUtf8();
	const auto keyU = DeriveKey(unicode, salt, 0, kFast);
	const auto slotsU = std::array<QByteArray, 2>{
		SealSlot(keyU, aad, SlotContent()),
		RandomBytes(kSlotSize),
	};
	AYU_CHECK(CheckPin(unicode, salt, kFast, filterId, slotsU).ok);
}

// The argument is the count of wrong PINs in a row: three free attempts,
// the fourth one waits 5 seconds.
void TestRetryDelay() {
	AYU_CHECK(RetryDelaySeconds(0) == 0);
	AYU_CHECK(RetryDelaySeconds(2) == 0);
	AYU_CHECK(RetryDelaySeconds(3) == 5);
	AYU_CHECK(RetryDelaySeconds(4) == 10);
	AYU_CHECK(RetryDelaySeconds(5) == 20);
	AYU_CHECK(RetryDelaySeconds(6) == 40);
	AYU_CHECK(RetryDelaySeconds(7) == 60);
	AYU_CHECK(RetryDelaySeconds(100) == 60);
}

// A broken record (bad KDF parameters) is a failure, not a wrong PIN.
void TestKdfFailure() {
	const auto salt = RandomBytes(kSaltSize);
	const auto broken = KdfParams{ .n = 0, .r = 8, .p = 1 };
	AYU_CHECK(DeriveKey("first-pin", salt, 0, broken).isEmpty());
	const auto slots = std::array<QByteArray, 2>{
		RandomBytes(kSlotSize),
		RandomBytes(kSlotSize),
	};
	const auto result = CheckPin("first-pin", salt, broken, 1, slots);
	AYU_CHECK(!result.ok && result.failed);
	const auto wrong = CheckPin("first-pin", salt, kFast, 1, slots);
	AYU_CHECK(!wrong.ok && !wrong.failed);
}

void TestAllowedSerialization() {
	const auto ids = std::vector<uint64_t>{ 1, 0xFFFFFFFFFFULL, 42 };
	const auto data = SerializeAllowed(ids);
	AYU_CHECK(data.size() == 1 + 2 + 8 * 3);
	const auto parsed = ParseAllowed(data);
	AYU_CHECK(parsed && *parsed == ids);
	AYU_CHECK(ParseAllowed(QByteArray()) && ParseAllowed(QByteArray())->empty());
	AYU_CHECK(!ParseAllowed(data.left(data.size() - 1)));
	auto bad = data;
	if (!bad.isEmpty()) {
		bad[0] = char(9);
	}
	AYU_CHECK(!ParseAllowed(bad));
}

void TestSecretSerialization() {
	const auto empty = ParseSecret(QByteArray());
	AYU_CHECK(empty && empty->decoyKey.isEmpty() && empty->allowed.empty());
	const auto secret = SecretData{
		.decoyKey = QByteArray(kKeySize, 'k'),
		.allowed = { 5, 6 },
	};
	const auto parsed = ParseSecret(SerializeSecret(secret));
	AYU_CHECK(parsed && parsed->decoyKey == secret.decoyKey);
	AYU_CHECK(parsed && parsed->allowed == secret.allowed);
	auto many = SecretData{ .decoyKey = QByteArray(kKeySize, 'k') };
	for (auto i = 0; i != kMaxAllowed; ++i) {
		many.allowed.push_back(uint64_t(i + 1));
	}
	const auto key = RandomBytes(kKeySize);
	const auto slot = SealSlot(key, MakeAad(1), SlotContent{
		.data = SerializeSecret(many),
	});
	AYU_CHECK(slot.size() == kSlotSize);
}

void TestCheckPinKeyAndEmptyPin() {
	const auto salt = RandomBytes(kSaltSize);
	const auto emptyKey = DeriveKey(QByteArray(), salt, 1, kFast);
	AYU_CHECK(emptyKey.size() == kKeySize);
	const auto slots = std::array<QByteArray, 2>{
		RandomBytes(kSlotSize),
		SealSlot(emptyKey, MakeAad(3), SlotContent{
			.role = Role::Decoy,
			.data = SerializeAllowed({ 7 }),
		}),
	};
	const auto result = CheckPin(QByteArray(), salt, kFast, 3, slots);
	AYU_CHECK(result.ok && result.slot == 1 && result.key == emptyKey);
	AYU_CHECK(!CheckPin("real-pin", salt, kFast, 3, slots).ok);
}

void TestMerge() {
	using namespace Ayu::FolderLockMerge;
	const auto real = Real{
		.always = { 1, 2, 3, 10 }, // 10 is hidden.
		.never = { 20 },
		.pinned = { 2, 10 }, // 10 is a hidden pinned chat.
	};
	// The decoy view showed allowed {1, 2, 4}, 4 is in the folder by type.
	// The user removed 2 and 4, added 5.
	const auto edit = Edit{
		.allowedOld = { 1, 2, 4 },
		.shownAlways = { 1, 5 },
		.shownNever = {},
		.typeMatched = { 4 },
	};
	const auto result = MergeDecoyEdit(real, edit);
	AYU_CHECK((result.always == std::set<uint64_t>{ 1, 3, 5, 10 }));
	AYU_CHECK((result.never == std::set<uint64_t>{ 4, 20 }));
	AYU_CHECK((result.pinned == std::vector<uint64_t>{ 10 }));
	AYU_CHECK((result.allowed == std::set<uint64_t>{ 1, 5 }));

	// Allowed chats outside of the folder rules now are kept (they come back
	// when the folder gets them again).
	auto withHidden = edit;
	withHidden.allowedOutside = { 30 };
	const auto kept = MergeDecoyEdit(real, withHidden);
	AYU_CHECK((kept.allowed == std::set<uint64_t>{ 1, 5, 30 }));
	AYU_CHECK(!kept.always.contains(30));
}

void TestWipe() {
	auto data = QByteArray("sensitive");
	Wipe(data);
	AYU_CHECK(data.isEmpty());
}

} // namespace

int main() {
	TestDeriveKey();
	TestSealOpen();
	TestCheckPin();
	TestRetryDelay();
	TestKdfFailure();
	TestAllowedSerialization();
	TestSecretSerialization();
	TestCheckPinKeyAndEmptyPin();
	TestMerge();
	TestWipe();
	if (Failed) {
		std::printf("FAILED: %d\n", Failed);
		return 1;
	}
	std::printf("ALL PASSED\n");
	return 0;
}
