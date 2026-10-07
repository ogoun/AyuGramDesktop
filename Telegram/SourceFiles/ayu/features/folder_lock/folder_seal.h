// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

class History;
class PeerData;
class DocumentData;
class PhotoData;

namespace Data {
struct FileOrigin;
} // namespace Data

namespace Main {
class Session;
} // namespace Main

// Chats of encrypted protected folders: nothing of them is written plain on
// the disk (media cache, drafts, recent search chats) and their media is not
// downloaded automatically. Thin wrappers for the tdesktop code.
namespace Ayu {

[[nodiscard]] bool IsSealedHistory(not_null<History*> history);
[[nodiscard]] bool IsSealedPeer(not_null<PeerData*> peer);
[[nodiscard]] bool IsSealedMedia(not_null<const DocumentData*> document);
[[nodiscard]] bool IsSealedMedia(not_null<const PhotoData*> photo);
[[nodiscard]] bool IsSealedOrigin(
	not_null<Main::Session*> session,
	const Data::FileOrigin &origin);

} // namespace Ayu
