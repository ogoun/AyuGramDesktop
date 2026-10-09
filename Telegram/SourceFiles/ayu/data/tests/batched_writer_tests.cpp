// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
//
// Unit tests of the batched database writer (target test_ayu_folder_lock).
#include "ayu/data/batched_writer.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace {

int Failed = 0;

#define AYU_CHECK(condition) do { \
	if (!(condition)) { \
		std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
		++Failed; \
	} \
} while (false)

using Ayu::BatchedWriter;

// Writes queued before a drain run as one batch, in order, and the drain
// is scheduled once per batch.
void TestBatching() {
	auto lock = std::recursive_mutex();
	auto scheduled = 0;
	auto batches = std::vector<int>();
	auto order = std::vector<int>();
	auto writer = BatchedWriter(
		lock,
		[&] { ++scheduled; },
		[&](std::vector<BatchedWriter::Write> &&writes) {
			batches.push_back(int(writes.size()));
			for (auto &write : writes) {
				write();
			}
		});
	for (auto i = 0; i != 3; ++i) {
		writer.enqueue([&, i] { order.push_back(i); });
	}
	AYU_CHECK(scheduled == 1);
	AYU_CHECK(writer.pending() == 3);
	AYU_CHECK(order.empty());
	writer.drain();
	AYU_CHECK(batches == std::vector<int>{ 3 });
	AYU_CHECK((order == std::vector<int>{ 0, 1, 2 }));
	AYU_CHECK(writer.pending() == 0);

	writer.drain(); // Nothing pending: no empty batch.
	AYU_CHECK(batches.size() == 1);

	writer.enqueue([&] { order.push_back(3); });
	AYU_CHECK(scheduled == 2); // A new batch schedules a new drain.
	writer.drain();
	AYU_CHECK((batches == std::vector<int>{ 3, 1 }));
	AYU_CHECK(order.back() == 3);
}

// After finish() writes run at once (nothing is left in the queue at exit).
void TestFinish() {
	auto lock = std::recursive_mutex();
	auto scheduled = 0;
	auto done = 0;
	auto writer = BatchedWriter(
		lock,
		[&] { ++scheduled; },
		[&](std::vector<BatchedWriter::Write> &&writes) {
			for (auto &write : writes) {
				write();
			}
		});
	writer.enqueue([&] { ++done; });
	writer.finish();
	AYU_CHECK(done == 1);
	writer.enqueue([&] { ++done; });
	AYU_CHECK(done == 2);
	AYU_CHECK(scheduled == 1);
	AYU_CHECK(writer.pending() == 0);
}

// Concurrent producers and drains: every write runs exactly once.
void TestThreads() {
	auto lock = std::recursive_mutex();
	auto executed = std::atomic<int>(0);
	auto writer = BatchedWriter(
		lock,
		[] {},
		[&](std::vector<BatchedWriter::Write> &&writes) {
			for (auto &write : writes) {
				write();
			}
		});
	auto threads = std::vector<std::thread>();
	for (auto t = 0; t != 4; ++t) {
		threads.emplace_back([&] {
			for (auto i = 0; i != 1000; ++i) {
				writer.enqueue([&] { ++executed; });
				if (i % 97 == 0) {
					writer.drain();
				}
			}
		});
	}
	for (auto &thread : threads) {
		thread.join();
	}
	writer.drain();
	AYU_CHECK(executed.load() == 4000);
}

// A drain under the database lock sees every write queued before it, even
// while the worker thread has taken a batch and waits for that lock.
void TestDrainUnderLock() {
	auto outer = std::recursive_mutex();
	auto &lock = outer;
	auto executed = std::atomic<int>(0);
	auto stop = std::atomic<bool>(false);
	auto writer = BatchedWriter(
		lock,
		[] {},
		[&](std::vector<BatchedWriter::Write> &&writes) {
			std::lock_guard lock(outer); // Like the database batch runner.
			std::this_thread::sleep_for(std::chrono::microseconds(200));
			for (auto &write : writes) {
				write();
			}
		});
	auto worker = std::thread([&] {
		while (!stop) {
			writer.drain();
		}
	});
	auto missed = 0;
	for (auto i = 0; i != 300; ++i) {
		writer.enqueue([&] { ++executed; });
		std::lock_guard lock(outer);
		writer.drain();
		if (executed.load() != i + 1) {
			++missed;
		}
	}
	stop = true;
	worker.join();
	AYU_CHECK(missed == 0);
}

} // namespace

int RunBatchedWriterTests() {
	TestBatching();
	TestFinish();
	TestThreads();
	TestDrainUnderLock();
	return Failed;
}
