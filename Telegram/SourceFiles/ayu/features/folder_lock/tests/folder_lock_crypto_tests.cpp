// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
//
// Unit tests of the folder lock crypto core (target test_ayu_folder_lock).
#include "ayu/features/folder_lock/folder_lock_crypto.h"

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

void TestRetryDelay() {
	AYU_CHECK(RetryDelaySeconds(0) == 0);
	AYU_CHECK(RetryDelaySeconds(3) == 0);
	AYU_CHECK(RetryDelaySeconds(4) == 5);
	AYU_CHECK(RetryDelaySeconds(5) == 10);
	AYU_CHECK(RetryDelaySeconds(6) == 20);
	AYU_CHECK(RetryDelaySeconds(7) == 40);
	AYU_CHECK(RetryDelaySeconds(8) == 60);
	AYU_CHECK(RetryDelaySeconds(100) == 60);
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
	TestWipe();
	if (Failed) {
		std::printf("FAILED: %d\n", Failed);
		return 1;
	}
	std::printf("ALL PASSED\n");
	return 0;
}
