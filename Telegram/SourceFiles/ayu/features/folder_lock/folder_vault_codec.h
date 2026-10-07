// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ayu/data/entities.h"

// Plain form of an encrypted AyuGram record (the contents of a sealed box).
namespace Ayu::FolderVaultCodec {

enum class Kind : unsigned char {
	Deleted = 1,
	Edited = 2,
};

struct Record {
	Kind kind = Kind::Deleted;
	AyuMessageBase message;
};

[[nodiscard]] QByteArray SerializeRecord(const Record &record);
[[nodiscard]] std::optional<Record> ParseRecord(const QByteArray &data);

// Overwrites the text fields before the record is freed (best effort).
void WipeRecord(Record &record);

} // namespace Ayu::FolderVaultCodec
