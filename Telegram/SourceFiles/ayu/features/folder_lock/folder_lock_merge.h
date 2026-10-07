// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include <cstdint>
#include <set>
#include <vector>

namespace Ayu::FolderLockMerge {

// Peer ids of the real (server) folder rules.
struct Real {
	std::set<uint64_t> always;
	std::set<uint64_t> never;
	std::vector<uint64_t> pinned;
};

// What the decoy (fake) folder settings showed and what the user left.
struct Edit {
	std::set<uint64_t> allowedOld;
	std::set<uint64_t> shownAlways;
	std::set<uint64_t> shownNever;
	std::set<uint64_t> typeMatched; // In the folder by type, not explicitly.
};

struct Result {
	std::set<uint64_t> always;
	std::set<uint64_t> never;
	std::set<uint64_t> allowed;
	std::vector<uint64_t> pinned;
};

// Applies an edit made in the decoy folder settings to the real folder
// rules, keeping the hidden part untouched.
[[nodiscard]] Result MergeDecoyEdit(const Real &real, const Edit &edit);

} // namespace Ayu::FolderLockMerge
