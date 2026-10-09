// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include <functional>
#include <mutex>
#include <vector>

namespace Ayu {

// Database writes off the main thread: writes queued until a drain run as
// one batch (one transaction), the drain is scheduled once per batch.
// Thread safe. drain() takes the database `lock` before it takes the batch,
// so a drain under that lock sees every write queued before it (a batch
// taken by another thread is already written).
class BatchedWriter final {
public:
	using Write = std::function<void()>;
	using Runner = std::function<void(std::vector<Write> &&writes)>;

	BatchedWriter(
		std::recursive_mutex &lock,
		std::function<void()> scheduleDrain,
		Runner run);

	void enqueue(Write write);
	// Runs the queued writes now: before reads and synchronous writes.
	void drain();
	// Drains and makes later writes run at once (application exit).
	void finish();
	[[nodiscard]] int pending() const;

private:
	std::recursive_mutex &_lock;
	const std::function<void()> _scheduleDrain;
	const Runner _run;
	mutable std::mutex _mutex;
	std::vector<Write> _pending;
	bool _scheduled = false;
	bool _finished = false;

};

} // namespace Ayu
