// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/folder_lock/folder_lock_merge.h"

namespace Ayu::FolderLockMerge {

Result MergeDecoyEdit(const Real &real, const Edit &edit) {
	auto removed = std::set<uint64_t>();
	for (const auto id : edit.allowedOld) {
		if (!edit.shownAlways.contains(id)) {
			removed.insert(id);
		}
	}
	auto added = std::set<uint64_t>();
	for (const auto id : edit.shownAlways) {
		if (!edit.allowedOld.contains(id)) {
			added.insert(id);
		}
	}
	auto result = Result();
	for (const auto id : real.always) {
		if (!removed.contains(id)) {
			result.always.insert(id);
		}
	}
	result.always.insert(begin(added), end(added));
	result.never = real.never;
	for (const auto id : removed) {
		if (edit.typeMatched.contains(id)) {
			result.never.insert(id); // In the folder by type: exclude it.
		}
	}
	for (const auto id : added) {
		result.never.erase(id);
	}
	result.never.insert(begin(edit.shownNever), end(edit.shownNever));
	for (const auto id : real.pinned) {
		if (!removed.contains(id)) {
			result.pinned.push_back(id);
		}
	}
	result.allowed = edit.shownAlways;
	return result;
}

} // namespace Ayu::FolderLockMerge
