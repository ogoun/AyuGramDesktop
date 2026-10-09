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
// Thread safe. The batch runner is called by drain() in the caller thread,
// so the caller holds the database lock around drain().
class BatchedWriter final {
public:
	using Write = std::function<void()>;
	using Runner = std::function<void(std::vector<Write> &&writes)>;

	BatchedWriter(std::function<void()> scheduleDrain, Runner run);

	void enqueue(Write write);
	// Runs the queued writes now: before reads and synchronous writes.
	void drain();
	// Drains and makes later writes run at once (application exit).
	void finish();
	[[nodiscard]] int pending() const;

private:
	const std::function<void()> _scheduleDrain;
	const Runner _run;
	mutable std::mutex _mutex;
	std::vector<Write> _pending;
	bool _scheduled = false;
	bool _finished = false;

};

} // namespace Ayu
