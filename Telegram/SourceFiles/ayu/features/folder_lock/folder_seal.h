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
// Folders are not loaded yet and some are encrypted: what is sealed is not
// known, plain data on the disk is neither written nor removed.
[[nodiscard]] bool IsSealUnknown(not_null<History*> history);
// Recent search chats: a chat is skipped only when it is known to be sealed.
[[nodiscard]] bool IsKnownSealedPeer(not_null<PeerData*> peer);

// Media created or updated inside the scope belongs to a sealed chat (its
// message is not registered yet): IsSealedMedia() is true for any media
// and the cloud file cache is skipped. Nesting is fine, main thread only.
class SealedScope final {
public:
	explicit SealedScope(bool sealed);
	SealedScope(const SealedScope &) = delete;
	SealedScope &operator=(const SealedScope &) = delete;
	~SealedScope();

	[[nodiscard]] static bool Active();

private:
	const bool _sealed = false;

};

} // namespace Ayu
