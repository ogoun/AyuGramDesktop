// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/data/batched_writer.h"

namespace Ayu {

BatchedWriter::BatchedWriter(
	std::recursive_mutex &lock,
	std::function<void()> scheduleDrain,
	Runner run)
: _lock(lock)
, _scheduleDrain(std::move(scheduleDrain))
, _run(std::move(run)) {
}

void BatchedWriter::enqueue(Write write) {
	auto schedule = false;
	auto now = std::vector<Write>();
	{
		std::lock_guard lock(_mutex);
		if (_finished) {
			now.push_back(std::move(write));
		} else {
			_pending.push_back(std::move(write));
			schedule = !_scheduled;
			_scheduled = true;
		}
	}
	if (!now.empty()) {
		std::lock_guard lock(_lock);
		_run(std::move(now));
	} else if (schedule) {
		_scheduleDrain();
	}
}

void BatchedWriter::drain() {
	std::lock_guard outer(_lock);
	auto writes = std::vector<Write>();
	{
		std::lock_guard lock(_mutex);
		writes.swap(_pending);
		_scheduled = false;
	}
	if (!writes.empty()) {
		_run(std::move(writes));
	}
}

void BatchedWriter::finish() {
	{
		std::lock_guard lock(_mutex);
		_finished = true;
	}
	drain();
}

int BatchedWriter::pending() const {
	std::lock_guard lock(_mutex);
	return int(_pending.size());
}

} // namespace Ayu
