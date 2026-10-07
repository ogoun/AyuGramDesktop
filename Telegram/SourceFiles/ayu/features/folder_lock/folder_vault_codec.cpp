// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/folder_lock/folder_vault_codec.h"

#include <QtCore/QDataStream>

#include <algorithm>
#include <array>

namespace Ayu::FolderVaultCodec {
namespace {

constexpr auto kVersion = char(1);
constexpr auto kMaxField = 64 * 1024 * 1024;

void WriteString(QDataStream &stream, const std::string &value) {
	stream << QByteArray(value.data(), int(value.size()));
}

void WriteBytes(QDataStream &stream, const std::vector<char> &value) {
	stream << QByteArray(value.data(), int(value.size()));
}

[[nodiscard]] bool ReadBytes(QDataStream &stream, QByteArray &value) {
	stream >> value;
	return (stream.status() == QDataStream::Ok) && (value.size() <= kMaxField);
}

[[nodiscard]] bool ReadString(QDataStream &stream, std::string &value) {
	auto bytes = QByteArray();
	if (!ReadBytes(stream, bytes)) {
		return false;
	}
	value.assign(bytes.constData(), size_t(bytes.size()));
	return true;
}

[[nodiscard]] bool ReadBytes(QDataStream &stream, std::vector<char> &value) {
	auto bytes = QByteArray();
	if (!ReadBytes(stream, bytes)) {
		return false;
	}
	value.assign(bytes.constData(), bytes.constData() + bytes.size());
	return true;
}

void Overwrite(std::string &value) {
	std::fill(value.begin(), value.end(), '\0');
	value.clear();
}

void Overwrite(std::vector<char> &value) {
	std::fill(value.begin(), value.end(), '\0');
	value.clear();
}

} // namespace

QByteArray SerializeRecord(const Record &record) {
	const auto &m = record.message;
	auto result = QByteArray();
	result.append(kVersion);
	result.append(char(record.kind));
	{
		auto stream = QDataStream(&result, QIODevice::Append);
		stream.setVersion(QDataStream::Qt_5_1);
		stream
			<< qint64(m.fakeId)
			<< qint64(m.userId)
			<< qint64(m.dialogId)
			<< qint64(m.groupedId)
			<< qint64(m.peerId)
			<< qint64(m.fromId)
			<< qint64(m.topicId)
			<< qint32(m.messageId)
			<< qint32(m.date)
			<< qint32(m.flags)
			<< qint32(m.editDate)
			<< qint32(m.views)
			<< qint32(m.fwdFlags)
			<< qint64(m.fwdFromId);
		WriteString(stream, m.fwdName);
		stream << qint32(m.fwdDate);
		WriteString(stream, m.fwdPostAuthor);
		WriteString(stream, m.postAuthor);
		stream
			<< qint32(m.replyFlags)
			<< qint32(m.replyMessageId)
			<< qint64(m.replyPeerId)
			<< qint32(m.replyTopId)
			<< qint32(m.replyForumTopic ? 1 : 0);
		WriteBytes(stream, m.replySerialized);
		WriteBytes(stream, m.replyMarkupSerialized);
		stream << qint32(m.entityCreateDate);
		WriteString(stream, m.text);
		WriteBytes(stream, m.textEntities);
		WriteString(stream, m.mediaPath);
		WriteString(stream, m.hqThumbPath);
		stream << qint32(m.documentType);
		WriteBytes(stream, m.documentSerialized);
		WriteBytes(stream, m.thumbsSerialized);
		WriteBytes(stream, m.documentAttributesSerialized);
		WriteString(stream, m.mimeType);
	}
	return result;
}

std::optional<Record> ParseRecord(const QByteArray &data) {
	if (data.size() < 2
		|| data[0] != kVersion
		|| (data[1] != char(Kind::Deleted) && data[1] != char(Kind::Edited))) {
		return std::nullopt;
	}
	auto result = Record{ .kind = Kind(uchar(data[1])) };
	auto &m = result.message;
	const auto body = data.mid(2);
	auto stream = QDataStream(body);
	stream.setVersion(QDataStream::Qt_5_1);
	auto i64 = std::array<qint64, 7>();
	auto i32 = std::array<qint32, 6>();
	for (auto &value : i64) {
		stream >> value;
	}
	for (auto &value : i32) {
		stream >> value;
	}
	auto fwdFromId = qint64();
	stream >> fwdFromId;
	m.fakeId = i64[0];
	m.userId = i64[1];
	m.dialogId = i64[2];
	m.groupedId = i64[3];
	m.peerId = i64[4];
	m.fromId = i64[5];
	m.topicId = i64[6];
	m.messageId = i32[0];
	m.date = i32[1];
	m.flags = i32[2];
	m.editDate = i32[3];
	m.views = i32[4];
	m.fwdFlags = i32[5];
	m.fwdFromId = fwdFromId;
	auto fwdDate = qint32();
	auto replyFlags = qint32();
	auto replyMessageId = qint32();
	auto replyPeerId = qint64();
	auto replyTopId = qint32();
	auto replyForumTopic = qint32();
	auto entityCreateDate = qint32();
	auto documentType = qint32();
	auto ok = ReadString(stream, m.fwdName);
	stream >> fwdDate;
	ok = ok
		&& ReadString(stream, m.fwdPostAuthor)
		&& ReadString(stream, m.postAuthor);
	stream
		>> replyFlags
		>> replyMessageId
		>> replyPeerId
		>> replyTopId
		>> replyForumTopic;
	ok = ok
		&& ReadBytes(stream, m.replySerialized)
		&& ReadBytes(stream, m.replyMarkupSerialized);
	stream >> entityCreateDate;
	ok = ok
		&& ReadString(stream, m.text)
		&& ReadBytes(stream, m.textEntities)
		&& ReadString(stream, m.mediaPath)
		&& ReadString(stream, m.hqThumbPath);
	stream >> documentType;
	ok = ok
		&& ReadBytes(stream, m.documentSerialized)
		&& ReadBytes(stream, m.thumbsSerialized)
		&& ReadBytes(stream, m.documentAttributesSerialized)
		&& ReadString(stream, m.mimeType);
	if (!ok || stream.status() != QDataStream::Ok || !stream.atEnd()) {
		WipeRecord(result);
		return std::nullopt;
	}
	m.fwdDate = fwdDate;
	m.replyFlags = replyFlags;
	m.replyMessageId = replyMessageId;
	m.replyPeerId = replyPeerId;
	m.replyTopId = replyTopId;
	m.replyForumTopic = (replyForumTopic != 0);
	m.entityCreateDate = entityCreateDate;
	m.documentType = documentType;
	return result;
}

void WipeRecord(Record &record) {
	auto &m = record.message;
	Overwrite(m.text);
	Overwrite(m.textEntities);
	Overwrite(m.fwdName);
	Overwrite(m.fwdPostAuthor);
	Overwrite(m.postAuthor);
	Overwrite(m.replySerialized);
	Overwrite(m.replyMarkupSerialized);
	Overwrite(m.mediaPath);
	Overwrite(m.hqThumbPath);
	Overwrite(m.documentSerialized);
	Overwrite(m.thumbsSerialized);
	Overwrite(m.documentAttributesSerialized);
	Overwrite(m.mimeType);
}

} // namespace Ayu::FolderVaultCodec
