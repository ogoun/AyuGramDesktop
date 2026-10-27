// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/utils/win_user_objects_tracker.h"

#ifdef Q_OS_WIN

#include <windows.h>
#include <delayimp.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace AyuDiagnostics {
namespace {

constexpr auto kMaxFrames = 32;
constexpr auto kSkipFrames = 2; // Record() and the hook itself.
constexpr auto kFirstDumpDelay = std::chrono::minutes(1);
constexpr auto kDumpInterval = std::chrono::minutes(10);
constexpr auto kTopByLive = 30;
constexpr auto kTopByGrowth = 20;
constexpr auto kMaxLogSize = DWORD(16 * 1024 * 1024); // Then rotate to .old.

enum class Kind {
	Window,
	Timer,
	Menu,
	Icon,
	Cursor,
	Hook,
	DeviceNotify,
	Count,
};

const char *KindName(Kind kind) {
	switch (kind) {
	case Kind::Window: return "window";
	case Kind::Timer: return "timer";
	case Kind::Menu: return "menu";
	case Kind::Icon: return "icon";
	case Kind::Cursor: return "cursor";
	case Kind::Hook: return "hook";
	case Kind::DeviceNotify: return "devnotify";
	default: return "?";
	}
}

struct Stack {
	Kind kind = Kind::Window;
	std::vector<void*> frames;
	std::wstring detail; // Window class of the first window, if any.
	int64_t live = 0;
	int64_t created = 0;
	int64_t liveAtLastDump = 0;
};

struct Object {
	uint64_t stack = 0;
};

using TimerKey = std::pair<HWND, UINT_PTR>;

struct State {
	std::mutex mutex;
	std::unordered_map<uint64_t, Stack> stacks;
	std::unordered_map<void*, Object> handles[size_t(Kind::Count)];
	std::map<TimerKey, Object> timers;
	std::wstring logPath;
	std::chrono::steady_clock::time_point started;
	int dumpIndex = 0;
};

State &S() {
	static auto result = new State(); // Never destroyed: hooks outlive statics.
	return *result;
}

struct Real {
	decltype(&::SetTimer) SetTimer = nullptr;
	decltype(&::KillTimer) KillTimer = nullptr;
	decltype(&::CreateWindowExW) CreateWindowExW = nullptr;
	decltype(&::CreateWindowExA) CreateWindowExA = nullptr;
	decltype(&::DestroyWindow) DestroyWindow = nullptr;
	decltype(&::CreatePopupMenu) CreatePopupMenu = nullptr;
	decltype(&::CreateMenu) CreateMenu = nullptr;
	decltype(&::DestroyMenu) DestroyMenu = nullptr;
	decltype(&::CreateIconIndirect) CreateIconIndirect = nullptr;
	decltype(&::DestroyIcon) DestroyIcon = nullptr;
	decltype(&::CreateCursor) CreateCursor = nullptr;
	decltype(&::DestroyCursor) DestroyCursor = nullptr;
	decltype(&::CopyImage) CopyImage = nullptr;
	decltype(&::LoadImageW) LoadImageW = nullptr;
	decltype(&::SetWindowsHookExW) SetWindowsHookExW = nullptr;
	decltype(&::UnhookWindowsHookEx) UnhookWindowsHookEx = nullptr;
	decltype(&::RegisterDeviceNotificationW) RegisterDeviceNotificationW = nullptr;
	decltype(&::UnregisterDeviceNotification) UnregisterDeviceNotification = nullptr;
} R;

uint64_t HashFrames(Kind kind, void **frames, USHORT count) {
	auto hash = uint64_t(1469598103934665603ULL) ^ uint64_t(kind);
	for (auto i = 0; i != count; ++i) {
		hash ^= uint64_t(frames[i]);
		hash *= 1099511628211ULL;
	}
	return hash;
}

uint64_t CaptureStack(Kind kind, const std::wstring *detail, State &s) {
	void *frames[kMaxFrames];
	const auto count = CaptureStackBackTrace(
		kSkipFrames,
		kMaxFrames,
		frames,
		nullptr);
	const auto hash = HashFrames(kind, frames, count);
	auto &stack = s.stacks[hash];
	if (stack.frames.empty()) {
		stack.kind = kind;
		stack.frames.assign(frames, frames + count);
		if (detail) {
			stack.detail = *detail;
		}
	}
	return hash;
}

void Added(Kind kind, void *handle, const std::wstring *detail = nullptr) {
	if (!handle) {
		return;
	}
	auto &s = S();
	auto lock = std::lock_guard(s.mutex);
	auto &map = s.handles[size_t(kind)];
	if (const auto i = map.find(handle); i != end(map)) {
		// Handle value reused after a destruction we didn't see.
		--s.stacks[i->second.stack].live;
		map.erase(i);
	}
	const auto hash = CaptureStack(kind, detail, s);
	auto &stack = s.stacks[hash];
	++stack.live;
	++stack.created;
	map.emplace(handle, Object{ hash });
}

void Removed(Kind kind, void *handle) {
	if (!handle) {
		return;
	}
	auto &s = S();
	auto lock = std::lock_guard(s.mutex);
	auto &map = s.handles[size_t(kind)];
	if (const auto i = map.find(handle); i != end(map)) {
		--s.stacks[i->second.stack].live;
		map.erase(i);
	}
}

// Must be called with the mutex locked.
void RemoveTimersOf(State &s, HWND hwnd) {
	auto i = s.timers.lower_bound({ hwnd, 0 });
	while (i != end(s.timers) && i->first.first == hwnd) {
		--s.stacks[i->second.stack].live;
		i = s.timers.erase(i);
	}
}

std::wstring ClassDetail(LPCWSTR className) {
	if (!className) {
		return {};
	} else if (!(ULONG_PTR(className) >> 16)) {
		return L"#atom" + std::to_wstring(ULONG_PTR(className));
	}
	return className;
}

std::wstring ClassDetail(LPCSTR className) {
	if (!className) {
		return {};
	} else if (!(ULONG_PTR(className) >> 16)) {
		return L"#atom" + std::to_wstring(ULONG_PTR(className));
	}
	auto result = std::wstring();
	for (auto p = className; *p; ++p) {
		result.push_back(wchar_t(uchar(*p)));
	}
	return result;
}

UINT_PTR WINAPI HookSetTimer(
		HWND hwnd,
		UINT_PTR id,
		UINT elapse,
		TIMERPROC proc) {
	const auto result = R.SetTimer(hwnd, id, elapse, proc);
	if (!result) {
		return result;
	}
	const auto key = TimerKey{ hwnd, hwnd ? id : result };
	auto &s = S();
	auto lock = std::lock_guard(s.mutex);
	if (s.timers.contains(key)) {
		return result; // Existing timer was reset, no new object.
	}
	const auto hash = CaptureStack(Kind::Timer, nullptr, s);
	auto &stack = s.stacks[hash];
	++stack.live;
	++stack.created;
	s.timers.emplace(key, Object{ hash });
	return result;
}

BOOL WINAPI HookKillTimer(HWND hwnd, UINT_PTR id) {
	const auto result = R.KillTimer(hwnd, id);
	auto &s = S();
	auto lock = std::lock_guard(s.mutex);
	if (const auto i = s.timers.find({ hwnd, id }); i != end(s.timers)) {
		--s.stacks[i->second.stack].live;
		s.timers.erase(i);
	}
	return result;
}

HWND WINAPI HookCreateWindowExW(
		DWORD exStyle,
		LPCWSTR className,
		LPCWSTR windowName,
		DWORD style,
		int x,
		int y,
		int width,
		int height,
		HWND parent,
		HMENU menu,
		HINSTANCE instance,
		LPVOID param) {
	const auto result = R.CreateWindowExW(
		exStyle,
		className,
		windowName,
		style,
		x,
		y,
		width,
		height,
		parent,
		menu,
		instance,
		param);
	const auto detail = ClassDetail(className);
	Added(Kind::Window, result, &detail);
	return result;
}

HWND WINAPI HookCreateWindowExA(
		DWORD exStyle,
		LPCSTR className,
		LPCSTR windowName,
		DWORD style,
		int x,
		int y,
		int width,
		int height,
		HWND parent,
		HMENU menu,
		HINSTANCE instance,
		LPVOID param) {
	const auto result = R.CreateWindowExA(
		exStyle,
		className,
		windowName,
		style,
		x,
		y,
		width,
		height,
		parent,
		menu,
		instance,
		param);
	const auto detail = ClassDetail(className);
	Added(Kind::Window, result, &detail);
	return result;
}

BOOL WINAPI HookDestroyWindow(HWND hwnd) {
	const auto result = R.DestroyWindow(hwnd);
	if (result) {
		Removed(Kind::Window, hwnd);
		auto &s = S();
		auto lock = std::lock_guard(s.mutex);
		RemoveTimersOf(s, hwnd);
	}
	return result;
}

HMENU WINAPI HookCreatePopupMenu() {
	const auto result = R.CreatePopupMenu();
	Added(Kind::Menu, result);
	return result;
}

HMENU WINAPI HookCreateMenu() {
	const auto result = R.CreateMenu();
	Added(Kind::Menu, result);
	return result;
}

BOOL WINAPI HookDestroyMenu(HMENU menu) {
	const auto result = R.DestroyMenu(menu);
	if (result) {
		Removed(Kind::Menu, menu);
	}
	return result;
}

HICON WINAPI HookCreateIconIndirect(PICONINFO info) {
	const auto result = R.CreateIconIndirect(info);
	Added((info && !info->fIcon) ? Kind::Cursor : Kind::Icon, result);
	return result;
}

BOOL WINAPI HookDestroyIcon(HICON icon) {
	const auto result = R.DestroyIcon(icon);
	if (result) {
		Removed(Kind::Icon, icon);
		Removed(Kind::Cursor, icon);
	}
	return result;
}

HCURSOR WINAPI HookCreateCursor(
		HINSTANCE instance,
		int xHotSpot,
		int yHotSpot,
		int width,
		int height,
		const VOID *andPlane,
		const VOID *xorPlane) {
	const auto result = R.CreateCursor(
		instance,
		xHotSpot,
		yHotSpot,
		width,
		height,
		andPlane,
		xorPlane);
	Added(Kind::Cursor, result);
	return result;
}

BOOL WINAPI HookDestroyCursor(HCURSOR cursor) {
	const auto result = R.DestroyCursor(cursor);
	if (result) {
		Removed(Kind::Cursor, cursor);
		Removed(Kind::Icon, cursor);
	}
	return result;
}

std::optional<Kind> ImageKind(UINT type) {
	switch (type) {
	case IMAGE_ICON: return Kind::Icon;
	case IMAGE_CURSOR: return Kind::Cursor;
	}
	return std::nullopt; // Bitmaps are GDI objects.
}

HANDLE WINAPI HookCopyImage(HANDLE h, UINT type, int cx, int cy, UINT flags) {
	const auto result = R.CopyImage(h, type, cx, cy, flags);
	if (const auto kind = ImageKind(type)) {
		if (result && (flags & LR_COPYDELETEORG)) {
			Removed(*kind, h);
		}
		if (result != h) {
			Added(*kind, result);
		}
	}
	return result;
}

HANDLE WINAPI HookLoadImageW(
		HINSTANCE instance,
		LPCWSTR name,
		UINT type,
		int cx,
		int cy,
		UINT flags) {
	const auto result = R.LoadImageW(instance, name, type, cx, cy, flags);
	if (const auto kind = ImageKind(type); kind && !(flags & LR_SHARED)) {
		Added(*kind, result);
	}
	return result;
}

HHOOK WINAPI HookSetWindowsHookExW(
		int id,
		HOOKPROC proc,
		HINSTANCE module,
		DWORD threadId) {
	const auto result = R.SetWindowsHookExW(id, proc, module, threadId);
	Added(Kind::Hook, result);
	return result;
}

BOOL WINAPI HookUnhookWindowsHookEx(HHOOK hook) {
	const auto result = R.UnhookWindowsHookEx(hook);
	if (result) {
		Removed(Kind::Hook, hook);
	}
	return result;
}

HDEVNOTIFY WINAPI HookRegisterDeviceNotificationW(
		HANDLE recipient,
		LPVOID filter,
		DWORD flags) {
	const auto result = R.RegisterDeviceNotificationW(
		recipient,
		filter,
		flags);
	Added(Kind::DeviceNotify, result);
	return result;
}

BOOL WINAPI HookUnregisterDeviceNotification(HDEVNOTIFY handle) {
	const auto result = R.UnregisterDeviceNotification(handle);
	if (result) {
		Removed(Kind::DeviceNotify, handle);
	}
	return result;
}

struct HookEntry {
	const char *name;
	void *hook;
	void **real;
};

std::vector<HookEntry> HookEntries() {
	const auto entry = [](const char *name, auto hook, auto &real) {
		return HookEntry{
			name,
			reinterpret_cast<void*>(hook),
			reinterpret_cast<void**>(&real),
		};
	};
	return {
		entry("SetTimer", &HookSetTimer, R.SetTimer),
		entry("KillTimer", &HookKillTimer, R.KillTimer),
		entry("CreateWindowExW", &HookCreateWindowExW, R.CreateWindowExW),
		entry("CreateWindowExA", &HookCreateWindowExA, R.CreateWindowExA),
		entry("DestroyWindow", &HookDestroyWindow, R.DestroyWindow),
		entry("CreatePopupMenu", &HookCreatePopupMenu, R.CreatePopupMenu),
		entry("CreateMenu", &HookCreateMenu, R.CreateMenu),
		entry("DestroyMenu", &HookDestroyMenu, R.DestroyMenu),
		entry(
			"CreateIconIndirect",
			&HookCreateIconIndirect,
			R.CreateIconIndirect),
		entry("DestroyIcon", &HookDestroyIcon, R.DestroyIcon),
		entry("CreateCursor", &HookCreateCursor, R.CreateCursor),
		entry("DestroyCursor", &HookDestroyCursor, R.DestroyCursor),
		entry("CopyImage", &HookCopyImage, R.CopyImage),
		entry("LoadImageW", &HookLoadImageW, R.LoadImageW),
		entry(
			"SetWindowsHookExW",
			&HookSetWindowsHookExW,
			R.SetWindowsHookExW),
		entry(
			"UnhookWindowsHookEx",
			&HookUnhookWindowsHookEx,
			R.UnhookWindowsHookEx),
		entry(
			"RegisterDeviceNotificationW",
			&HookRegisterDeviceNotificationW,
			R.RegisterDeviceNotificationW),
		entry(
			"UnregisterDeviceNotification",
			&HookUnregisterDeviceNotification,
			R.UnregisterDeviceNotification),
	};
}

void PatchThunks(
		BYTE *base,
		IMAGE_THUNK_DATA *names,
		IMAGE_THUNK_DATA *addresses,
		const std::vector<HookEntry> &entries) {
	for (; names->u1.AddressOfData; ++names, ++addresses) {
		if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
			continue;
		}
		const auto byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
			base + names->u1.AddressOfData);
		const auto name = reinterpret_cast<const char*>(byName->Name);
		for (const auto &entry : entries) {
			if (strcmp(name, entry.name) || !*entry.real) {
				continue;
			}
			auto old = DWORD();
			auto slot = reinterpret_cast<void**>(&addresses->u1.Function);
			if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
				*slot = entry.hook;
				VirtualProtect(slot, sizeof(void*), old, &old);
			}
		}
	}
}

bool IsUser32(const char *name) {
	return !_stricmp(name, "user32.dll");
}

void PatchImports(const std::vector<HookEntry> &entries) {
	const auto base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
	const auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
	const auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
	const auto &dirs = nt->OptionalHeader.DataDirectory;

	const auto &imports = dirs[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (imports.VirtualAddress) {
		for (auto d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
				base + imports.VirtualAddress); d->Name; ++d) {
			if (IsUser32(reinterpret_cast<const char*>(base + d->Name))
				&& d->OriginalFirstThunk) {
				PatchThunks(
					base,
					reinterpret_cast<IMAGE_THUNK_DATA*>(
						base + d->OriginalFirstThunk),
					reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk),
					entries);
			}
		}
	}

	// Most system libraries are delay-loaded in this build.
	const auto &delayed = dirs[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
	if (delayed.VirtualAddress) {
		for (auto d = reinterpret_cast<ImgDelayDescr*>(
				base + delayed.VirtualAddress); d->rvaDLLName; ++d) {
			if (IsUser32(reinterpret_cast<const char*>(base + d->rvaDLLName))) {
				PatchThunks(
					base,
					reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->rvaINT),
					reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->rvaIAT),
					entries);
			}
		}
	}
}

std::string Narrow(const std::wstring &value) {
	auto result = std::string();
	for (const auto ch : value) {
		result.push_back((ch > 0 && ch < 128) ? char(ch) : '?');
	}
	return result;
}

std::string FrameName(void *address) {
	static auto modules = std::map<HMODULE, std::string>();
	auto module = HMODULE();
	if (!GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
				| GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(address),
			&module)) {
		char buffer[32];
		sprintf_s(buffer, "0x%p", address);
		return buffer;
	}
	auto &name = modules[module];
	if (name.empty()) {
		wchar_t path[MAX_PATH] = { 0 };
		GetModuleFileNameW(module, path, MAX_PATH);
		const auto full = std::wstring(path);
		const auto slash = full.find_last_of(L"\\/");
		name = Narrow(slash == std::wstring::npos
			? full
			: full.substr(slash + 1));
	}
	char buffer[64];
	sprintf_s(
		buffer,
		"+0x%llx",
		static_cast<unsigned long long>(
			reinterpret_cast<BYTE*>(address)
				- reinterpret_cast<BYTE*>(module)));
	return name + buffer;
}

struct WindowCensus {
	std::map<std::wstring, std::pair<int, std::map<DWORD, int>>> byClass;
	int total = 0;
};

void CountWindow(HWND hwnd, WindowCensus &census) {
	auto pid = DWORD();
	const auto thread = GetWindowThreadProcessId(hwnd, &pid);
	if (pid != GetCurrentProcessId()) {
		return;
	}
	wchar_t name[256] = { 0 };
	GetClassNameW(hwnd, name, 256);
	auto &entry = census.byClass[name];
	++entry.first;
	++entry.second[thread];
	++census.total;
}

BOOL CALLBACK CountChild(HWND hwnd, LPARAM param) {
	CountWindow(hwnd, *reinterpret_cast<WindowCensus*>(param));
	return TRUE;
}

BOOL CALLBACK CountTopLevel(HWND hwnd, LPARAM param) {
	auto pid = DWORD();
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid == GetCurrentProcessId()) {
		CountWindow(hwnd, *reinterpret_cast<WindowCensus*>(param));
		EnumChildWindows(hwnd, CountChild, param);
	}
	return TRUE;
}

WindowCensus TakeCensus() {
	auto result = WindowCensus();
	EnumWindows(CountTopLevel, LPARAM(&result));
	// Message-only windows aren't enumerated by EnumWindows.
	auto hwnd = HWND();
	while ((hwnd = FindWindowExW(HWND_MESSAGE, hwnd, nullptr, nullptr))) {
		CountWindow(hwnd, result);
	}
	return result;
}

// Must be called with the mutex locked.
void Sweep(State &s) {
	auto &windows = s.handles[size_t(Kind::Window)];
	for (auto i = begin(windows); i != end(windows);) {
		if (!IsWindow(static_cast<HWND>(i->first))) {
			--s.stacks[i->second.stack].live;
			i = windows.erase(i);
		} else {
			++i;
		}
	}
	for (auto i = begin(s.timers); i != end(s.timers);) {
		if (i->first.first && !IsWindow(i->first.first)) {
			--s.stacks[i->second.stack].live;
			i = s.timers.erase(i);
		} else {
			++i;
		}
	}
	auto &menus = s.handles[size_t(Kind::Menu)];
	for (auto i = begin(menus); i != end(menus);) {
		if (!IsMenu(static_cast<HMENU>(i->first))) {
			--s.stacks[i->second.stack].live;
			i = menus.erase(i);
		} else {
			++i;
		}
	}
}

void Dump() {
	auto &s = S();
	const auto census = TakeCensus();
	const auto process = GetCurrentProcess();
	const auto user = GetGuiResources(process, GR_USEROBJECTS);
	const auto gdi = GetGuiResources(process, GR_GDIOBJECTS);
	const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::steady_clock::now() - s.started).count();

	auto stacks = std::vector<std::pair<uint64_t, Stack>>();
	auto lines = std::string();
	char buffer[512];
	{
		auto lock = std::lock_guard(s.mutex);
		Sweep(s);
		stacks.reserve(s.stacks.size());
		for (auto &[hash, stack] : s.stacks) {
			stacks.emplace_back(hash, stack);
			stack.liveAtLastDump = stack.live;
		}
	}
	int64_t liveByKind[size_t(Kind::Count)] = { 0 };
	for (const auto &[hash, stack] : stacks) {
		liveByKind[size_t(stack.kind)] += stack.live;
	}

	SYSTEMTIME time;
	GetLocalTime(&time);
	sprintf_s(
		buffer,
		"\n===== dump %d at %04d-%02d-%02d %02d:%02d:%02d, uptime %llds,"
		" USER=%lu GDI=%lu, windows of process=%d\n",
		++s.dumpIndex,
		time.wYear,
		time.wMonth,
		time.wDay,
		time.wHour,
		time.wMinute,
		time.wSecond,
		static_cast<long long>(uptime),
		user,
		gdi,
		census.total);
	lines += buffer;
	lines += "tracked live:";
	for (auto i = 0; i != int(Kind::Count); ++i) {
		sprintf_s(
			buffer,
			" %s=%lld",
			KindName(Kind(i)),
			static_cast<long long>(liveByKind[i]));
		lines += buffer;
	}
	lines += "\n\n-- window census by class (count, threads) --\n";
	auto classes = std::vector<std::pair<int, std::wstring>>();
	for (const auto &[name, entry] : census.byClass) {
		classes.emplace_back(entry.first, name);
	}
	std::sort(classes.rbegin(), classes.rend());
	for (const auto &[count, name] : classes) {
		sprintf_s(
			buffer,
			"%6d  %-60s threads=%d\n",
			count,
			Narrow(name).c_str(),
			int(census.byClass.at(name).second.size()));
		lines += buffer;
	}

	const auto print = [&](const char *title, auto &&compare, int limit) {
		std::sort(begin(stacks), end(stacks), compare);
		lines += "\n-- ";
		lines += title;
		lines += " --\n";
		for (auto i = 0; i != std::min(limit, int(stacks.size())); ++i) {
			const auto &[hash, stack] = stacks[i];
			if (stack.live <= 0) {
				break;
			}
			sprintf_s(
				buffer,
				"#%d %s live=%lld created=%lld growth=%+lld %s\n",
				i + 1,
				KindName(stack.kind),
				static_cast<long long>(stack.live),
				static_cast<long long>(stack.created),
				static_cast<long long>(stack.live - stack.liveAtLastDump),
				Narrow(stack.detail).c_str());
			lines += buffer;
			for (const auto frame : stack.frames) {
				lines += "    " + FrameName(frame) + "\n";
			}
		}
	};
	print("stacks by live objects", [](const auto &a, const auto &b) {
		return a.second.live > b.second.live;
	}, kTopByLive);
	print("stacks by growth since previous dump", [](
			const auto &a,
			const auto &b) {
		return (a.second.live - a.second.liveAtLastDump)
			> (b.second.live - b.second.liveAtLastDump);
	}, kTopByGrowth);

	auto attributes = WIN32_FILE_ATTRIBUTE_DATA();
	if (GetFileAttributesExW(
			s.logPath.c_str(),
			GetFileExInfoStandard,
			&attributes)
		&& (attributes.nFileSizeHigh || attributes.nFileSizeLow > kMaxLogSize)) {
		MoveFileExW(
			s.logPath.c_str(),
			(s.logPath + L".old").c_str(),
			MOVEFILE_REPLACE_EXISTING);
	}
	auto file = (FILE*)nullptr;
	if (!_wfopen_s(&file, s.logPath.c_str(), L"ab") && file) {
		fwrite(lines.data(), 1, lines.size(), file);
		fclose(file);
	}
}

} // namespace

void InstallUserObjectsTracker(const QString &workingDir) {
	static auto installed = false;
	if (installed) {
		return;
	}
	installed = true;

	auto &s = S();
	s.started = std::chrono::steady_clock::now();
	s.logPath = (workingDir + u"user_objects.log"_q).toStdWString();

	const auto user32 = GetModuleHandleW(L"user32.dll");
	if (!user32) {
		return;
	}
	const auto entries = HookEntries();
	for (const auto &entry : entries) {
		*entry.real = reinterpret_cast<void*>(
			GetProcAddress(user32, entry.name));
	}
	PatchImports(entries);

	// A plain thread: Qt timers stop working once the quota is exhausted.
	std::thread([] {
		std::this_thread::sleep_for(kFirstDumpDelay);
		while (true) {
			Dump();
			std::this_thread::sleep_for(kDumpInterval);
		}
	}).detach();
}

} // namespace AyuDiagnostics

#else // Q_OS_WIN

namespace AyuDiagnostics {

void InstallUserObjectsTracker(const QString &workingDir) {
}

} // namespace AyuDiagnostics

#endif // Q_OS_WIN
