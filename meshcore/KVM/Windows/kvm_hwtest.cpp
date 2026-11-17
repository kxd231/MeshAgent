// KVM (Windows): the self test of the hardware H.264 encoder as a process, the cache file and the switch files, see kvm_hwtest.h.
// This file must not include microstack or agent headers.

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <time.h>

#include "kvm_hwtest.h"
#include "kvm_hwtest_core.h"
int mfp_os_build(void);	// mf_h264.cpp (the build number of Windows)
HMODULE mfp_load_system_dll(const wchar_t *name);	// mf_h264.cpp (a system library from the system directory)

// ------------------------------------------------------------------------------------------------------------------------ paths and files

static void file_path(wchar_t *out, size_t cap, const wchar_t *suffix)
{
	wchar_t exe[MAX_PATH + 4];
	const DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
	out[0] = 0;
	if (n == 0 || n >= MAX_PATH) { return; }
	exe[n] = 0;
	{
		wchar_t *dot = wcsrchr(exe, L'.');
		wchar_t *sl = wcsrchr(exe, L'\\');
		if (dot != NULL && (sl == NULL || dot > sl)) { *dot = 0; }
	}
	_snwprintf(out, cap, L"%s%s", exe, suffix);
	out[cap - 1] = 0;
}

static int read_file(const wchar_t *path, char *buf, size_t cap, size_t *n)
{
	HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD got = 0;
	*n = 0;
	if (h == INVALID_HANDLE_VALUE) { return -1; }
	if (ReadFile(h, buf, (DWORD)(cap - 1), &got, NULL)) { *n = got; buf[got] = 0; }
	CloseHandle(h);
	return 0;
}

static int write_file_atomic(const wchar_t *path, const char *data, size_t n)
{
	wchar_t tmp[MAX_PATH + 16];
	HANDLE h;
	DWORD put = 0;
	int ok;
	_snwprintf(tmp, sizeof(tmp) / sizeof(tmp[0]), L"%s.tmp", path);
	tmp[sizeof(tmp) / sizeof(tmp[0]) - 1] = 0;
	h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) { return -1; }
	ok = WriteFile(h, data, (DWORD)n, &put, NULL) && put == (DWORD)n && FlushFileBuffers(h);
	CloseHandle(h);
	if (!ok) { DeleteFileW(tmp); return -1; }
	if (!MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { DeleteFileW(tmp); return -1; }
	return 0;
}

int kvm_hwcache_read(kvm_hwp_cache *c)
{
	wchar_t path[MAX_PATH + 32];
	char buf[8192];
	size_t n = 0;
	memset(c, 0, sizeof(*c));
	file_path(path, sizeof(path) / sizeof(path[0]), L"-kvmhw.txt");
	if (path[0] == 0 || read_file(path, buf, sizeof(buf), &n) != 0) { return -1; }
	kvm_hwp_parse(c, buf, n);
	return 0;
}

int kvm_hwcache_update(kvm_hwcache_fn fn, void *arg)
{
	wchar_t path[MAX_PATH + 32];
	char buf[8192];
	size_t n = 0;
	kvm_hwp_cache c;
	int r = -1;
	HANDLE mx = CreateMutexW(NULL, FALSE, L"Global\\MeshKvmHwCache");	// (SYSTEM creates it, every KVM child is SYSTEM)
	if (mx == NULL) { return -1; }
	{ const DWORD w = WaitForSingleObject(mx, 3000); if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED)
	{
		// (an abandoned mutex, a child that died inside, is ours now: the file is replaced as a whole, so it is whole)
		CloseHandle(mx);
		return -1;
	} }
	file_path(path, sizeof(path) / sizeof(path[0]), L"-kvmhw.txt");
	memset(&c, 0, sizeof(c));
	if (path[0] != 0)
	{
		if (read_file(path, buf, sizeof(buf), &n) == 0) { kvm_hwp_parse(&c, buf, n); }
		if (fn(&c, arg))
		{
			const size_t m = kvm_hwp_format(&c, buf, sizeof(buf));
			r = (m > 0 && write_file_atomic(path, buf, m) == 0) ? 0 : -1;
		}
		else { r = 0; }
	}
	ReleaseMutex(mx);
	CloseHandle(mx);
	return r;
}

int kvm_hwmode_read(void)
{
	wchar_t path[MAX_PATH + 32];
	char buf[64];
	size_t n = 0;
	file_path(path, sizeof(path) / sizeof(path[0]), L"-kvmhw-mode.txt");
	if (path[0] == 0 || read_file(path, buf, sizeof(buf), &n) != 0) { return KVM_HWP_MODE_AUTO; }
	return kvm_hwp_mode_parse(buf, n);
}

int kvm_capturefile_read(void)
{
	wchar_t path[MAX_PATH + 32];
	char buf[64];
	size_t n = 0;
	file_path(path, sizeof(path) / sizeof(path[0]), L"-kvm-capture.txt");
	if (path[0] == 0 || read_file(path, buf, sizeof(buf), &n) != 0) { return 0; }
	return kvm_hwp_capture_parse(buf, n);
}

int kvm_hwenv_read(void)
{
	char v[16] = "";
	if (GetEnvironmentVariableA("MESH_KVM_VIDEO_HW", v, sizeof(v)) == 0) { return KVM_HWP_ENV_AUTO; }		// (not set, or empty)
	return kvm_hwp_env_parse(v);
}

int kvm_hw_pid_alive(uint32_t pid)
{
	HANDLE h;
	DWORD code = 0;
	int alive = 0;
	if (pid == 0 || pid == GetCurrentProcessId()) { return 0; }
	h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (h == NULL) { return 0; }
	if (GetExitCodeProcess(h, &code) && code == STILL_ACTIVE) { alive = 1; }
	CloseHandle(h);
	return alive;
}

// ------------------------------------------------------------------------------------------------------------------------ the parent and the marker of a try

typedef struct child_gone_arg { uint32_t pid, exitCode; } child_gone_arg;
static int child_gone_fn(kvm_hwp_cache *c, void *p)
{
	const child_gone_arg *a = (const child_gone_arg *)p;
	return kvm_hwp_resolve_try(c, a->pid, a->exitCode, (uint32_t)(time(NULL) & 0xFFFFFFFFu));
}

// The KVM child with this process id has ended (or is about to be killed by the parent: exitCode 1067) with this exit code: the marker "the hardware is in use" is its to clear, or, when the code is a status of
// a crash (>= 0xC0000000, except the two of a logoff) or 0x4857, an exception of weight 2 (kvm_hwp_resolve_try). The parent calls this at the end of every KVM session and when the child exits by itself; cheap
// when there is no marker. Returns bits for the log: 1 = the child had the marker, 2 = it was counted as a fault of the hardware, 4 = the code is worth a line in the log (a status of a crash, 0x4857).
// 0x4857 is only ever given by a child that ended itself after an exception in the driver: the children get MESH_KVM_VIDEO_HW=0 whatever the cache says (it may not be writable, or the marker may be missing).
int kvm_hwtest_child_gone(uint32_t pid, uint32_t exitCode)
{
	child_gone_arg a;
	kvm_hwp_cache c;
	int bits = (exitCode >= KVM_HWP_CRASH_STATUS || exitCode == KVM_HWP_EXC_EXIT) ? 4 : 0;
	const int counts = kvm_hwp_exit_counts(exitCode);
	a.pid = pid; a.exitCode = exitCode;
	if (counts && exitCode == KVM_HWP_EXC_EXIT) { SetEnvironmentVariableA("MESH_KVM_VIDEO_HW", "0"); }
	if (kvm_hwcache_read(&c) != 0 || !c.trying || c.tryPid != pid) { return bits; }	// (no file or no marker of this child: nothing to write)
	bits |= 1;
	if (counts) { bits |= 2; }
	if (kvm_hwcache_update(child_gone_fn, &a) != 0 && counts)
	{
		// the counter of a crash could not be written (the file or its mutex): the next child must not use the hardware either, or the restarts would never end. The environment of this process (the service) is
		// inherited by the children; it is the switch MESH_KVM_VIDEO_HW=0 and lasts until the service is restarted.
		SetEnvironmentVariableA("MESH_KVM_VIDEO_HW", "0");
	}
	return bits;
}

// ------------------------------------------------------------------------------------------------------------------------ key

int kvm_hwtest_token_class(void)
{
	HANDLE tok = NULL;
	int cls = KVM_HWTEST_TOKEN_OTHER;
	unsigned char buf[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
	DWORD need = 0, session = 0;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) { return cls; }
	if (GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &need))
	{
		SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
		PSID sys = NULL;
		if (AllocateAndInitializeSid(&nt, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0, &sys))
		{
			const int isSystem = EqualSid(((TOKEN_USER *)buf)->User.Sid, sys) ? 1 : 0;
			FreeSid(sys);
			if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) { session = 0; }
			cls = isSystem ? (session != 0 ? KVM_HWTEST_TOKEN_SYSTEM_IN_SESSION : KVM_HWTEST_TOKEN_OTHER) : KVM_HWTEST_TOKEN_USER;
		}
	}
	CloseHandle(tok);
	return cls;
}

#ifndef KVM_HW_SELECT_MS
#define KVM_HW_SELECT_MS 3000	// the enumeration of the encoders and adapters may take this long; a hang is an error of the machine, not of the video
#endif

int kvm_hwtest_select(int64_t luid, kvm_mfhw_sel *sel)
{
	return kvm_mfhw_select_timed(luid, sel, KVM_HW_SELECT_MS);
}

int kvm_hwtest_key_from_sel(const kvm_mfhw_sel *sel, unsigned codedW, unsigned codedH, char *key, size_t cap)
{
	kvm_hwp_sig sig;
	if (key != NULL && cap > 0) { key[0] = 0; }
	if (sel == NULL || !sel->found) { return 0; }
	memset(&sig, 0, sizeof(sig));
	sig.rev = KVM_HW_SELFTEST_REV;
	snprintf(sig.name, sizeof(sig.name), "%s", sel->name);
	snprintf(sig.clsid, sizeof(sig.clsid), "%s", sel->clsid);
	sig.vendor = sel->vendor; sig.device = sel->device; sig.subsys = sel->subsys; sig.adapterRev = sel->adapterRev;
	snprintf(sig.driver, sizeof(sig.driver), "%s", sel->driver);
	sig.osBuild = (unsigned)mfp_os_build();
	sig.tokenClass = kvm_hwtest_token_class();
	sig.sizeClass = kvm_hwp_size_class(codedW, codedH);
	return kvm_hwp_key_build(&sig, key, cap) > 0 ? 1 : 0;
}

int kvm_hwtest_make_key(int64_t luid, unsigned codedW, unsigned codedH, char *key, size_t cap, kvm_mfhw_sel *selOut)
{
	kvm_mfhw_sel sel;
	if (key != NULL && cap > 0) { key[0] = 0; }
	if (kvm_hwtest_select(luid, &sel) != 1 || !sel.found) { if (selOut != NULL) { memset(selOut, 0, sizeof(*selOut)); } return 0; }
	if (selOut != NULL) { *selOut = sel; }
	return kvm_hwtest_key_from_sel(&sel, codedW, codedH, key, cap);
}

// The key of the entry that says "the enumeration hangs on this machine" (what is known without the enumeration: the adapter of the captured output, the build of Windows, the token)
int kvm_hwtest_select_key(unsigned vendor, unsigned device, unsigned subsys, unsigned adapterRev, char *key, size_t cap)
{
	const int w = snprintf(key, cap, "sel,rev:%d,ven:%04x,dev:%04x,sub:%08x,arev:%02x,os:%u,tok:%d", KVM_HW_SELFTEST_REV, vendor & 0xFFFFu, device & 0xFFFFu, subsys, adapterRev & 0xFFu, (unsigned)mfp_os_build(), kvm_hwtest_token_class());
	if (w < 0 || (size_t)w >= cap) { if (cap > 0) { key[0] = 0; } return 0; }
	return 1;
}

// ------------------------------------------------------------------------------------------------------------------------ the process of the test, parent side

// Milliseconds that do not count the time that the machine slept (QueryUnbiasedInterruptTime, Windows 7 and later, 100 ns units): a laptop that is put to sleep during the test is no timeout of the encoder.
static unsigned long long unbiased_ms(void)
{
	typedef BOOL(WINAPI * PFN_QUIT)(PULONGLONG);
	static PFN_QUIT f = NULL;
	static int tried = 0;
	ULONGLONG t = 0;
	if (!tried) { HMODULE k = GetModuleHandleW(L"kernel32.dll"); f = k != NULL ? (PFN_QUIT)GetProcAddress(k, "QueryUnbiasedInterruptTime") : NULL; tried = 1; }
	if (f != NULL && f(&t)) { return (unsigned long long)(t / 10000ull); }
	return (unsigned long long)GetTickCount64();	// (Windows before 7: the time of sleep counts, as it did)
}

int kvm_hwtest_start(kvm_hwtest_run *r, unsigned codedW, unsigned codedH, int level, int maxLevel, int profile, int64_t luid, const char *key)
{
	wchar_t exe[MAX_PATH + 4], cmd[MAX_PATH + 512];
	STARTUPINFOEXW si;
	PROCESS_INFORMATION pi;
	SECURITY_ATTRIBUTES sa;
	LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
	SIZE_T attrSize = 0;
	HANDLE inherit[2];
	HANDLE rd = NULL, wr = NULL, nul = INVALID_HANDLE_VALUE, job = NULL;
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
	wchar_t wkey[KVM_HWP_KEY_MAX + 1];
	size_t i;
	const DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
	memset(r, 0, sizeof(*r));
	if (n == 0 || n >= MAX_PATH || key == NULL || strlen(key) > KVM_HWP_KEY_MAX) { return -1; }
	exe[n] = 0;
	for (i = 0; key[i] != 0; i++) { wkey[i] = (wchar_t)(unsigned char)key[i]; }
	wkey[i] = 0;
	_snwprintf(cmd, sizeof(cmd) / sizeof(cmd[0]), L"\"%s\" -kvmhwtest %u %u %d %d %d %lld %d %s", exe, codedW, codedH, level, maxLevel, profile, (long long)luid, KVM_HW_SELFTEST_REV, wkey);
	cmd[sizeof(cmd) / sizeof(cmd[0]) - 1] = 0;
	memset(&sa, 0, sizeof(sa));
	sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
	if (!CreatePipe(&rd, &wr, &sa, 0)) { return -1; }
	SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
	nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
	if (nul == INVALID_HANDLE_VALUE) { goto fail; }
	memset(&si, 0, sizeof(si));
	// stdout is the pipe of the result; stdin and STDERR are NUL (what the test writes to stderr must not eat the buffer of the result). lpDesktop stays NULL: the desktop of the child.
	si.StartupInfo.cb = sizeof(si); si.StartupInfo.dwFlags = STARTF_USESTDHANDLES; si.StartupInfo.hStdInput = nul; si.StartupInfo.hStdOutput = wr; si.StartupInfo.hStdError = nul;
	// the test inherits exactly these two handles and nothing else of the child (not the pipes of the KVM channel to the service)
	inherit[0] = wr; inherit[1] = nul;
	InitializeProcThreadAttributeList(NULL, 1, 0, &attrSize);
	attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, attrSize);
	if (attrs == NULL || !InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize)) { goto fail; }
	if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), NULL, NULL)) { goto fail; }
	si.lpAttributeList = attrs;
	memset(&pi, 0, sizeof(pi));
	job = CreateJobObjectW(NULL, NULL);
	if (job == NULL) { goto fail; }
	memset(&li, 0, sizeof(li));
	li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
	li.BasicLimitInformation.ActiveProcessLimit = 1;
	li.ProcessMemoryLimit = (SIZE_T)KVM_HWTEST_MEMORY_MB * 1024u * 1024u;
	if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li))) { goto fail; }
	if (!CreateProcessW(exe, cmd, NULL, NULL, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &si.StartupInfo, &pi)) { goto fail; }
	if (!AssignProcessToJobObject(job, pi.hProcess)) { TerminateProcess(pi.hProcess, 9); CloseHandle(pi.hThread); CloseHandle(pi.hProcess); goto fail; }
	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);
	CloseHandle(wr);
	if (nul != INVALID_HANDLE_VALUE) { CloseHandle(nul); }
	DeleteProcThreadAttributeList(attrs); HeapFree(GetProcessHeap(), 0, attrs);
	r->proc = pi.hProcess; r->job = job; r->pipe = rd; r->startedMs = unbiased_ms(); r->running = 1;
	return 0;
fail:
	if (attrs != NULL) { DeleteProcThreadAttributeList(attrs); HeapFree(GetProcessHeap(), 0, attrs); }
	if (rd != NULL) { CloseHandle(rd); }
	if (wr != NULL) { CloseHandle(wr); }
	if (nul != INVALID_HANDLE_VALUE) { CloseHandle(nul); }
	if (job != NULL) { CloseHandle(job); }
	memset(r, 0, sizeof(*r));
	return -1;
}

static void pump_pipe(kvm_hwtest_run *r)
{
	DWORD avail = 0, got = 0;
	while (r->pipe != NULL && PeekNamedPipe((HANDLE)r->pipe, NULL, 0, NULL, &avail, NULL) && avail > 0)
	{
		char tmp[512];
		if (!ReadFile((HANDLE)r->pipe, tmp, avail > sizeof(tmp) ? (DWORD)sizeof(tmp) : avail, &got, NULL) || got == 0) { break; }
		if (r->n + got < sizeof(r->buf)) { memcpy(r->buf + r->n, tmp, got); r->n += got; }	// (more than the buffer: the rest is dropped, the result is a few hundred bytes)
	}
}

static void release_run(kvm_hwtest_run *r)
{
	if (r->proc != NULL) { CloseHandle((HANDLE)r->proc); }
	if (r->pipe != NULL) { CloseHandle((HANDLE)r->pipe); }
	if (r->job != NULL) { CloseHandle((HANDLE)r->job); }		// KILL_ON_JOB_CLOSE: nothing of the test is left
	r->proc = r->pipe = r->job = NULL;
	r->running = 0;
}

int kvm_hwtest_poll(kvm_hwtest_run *r, int *verdict, kvm_hwp_result *res)
{
	DWORD code = 0;
	int timedOut = 0;
	if (!r->running) { return 0; }
	pump_pipe(r);
	if (WaitForSingleObject((HANDLE)r->proc, 0) != WAIT_OBJECT_0)
	{
		if (unbiased_ms() - r->startedMs < (unsigned long long)KVM_HWTEST_TIMEOUT_MS) { return 0; }
		if (!r->killed) { TerminateJobObject((HANDLE)r->job, 9); r->killed = 1; }	// (the loop of the KVM child does not wait for the process to go: the next poll sees it)
		if (WaitForSingleObject((HANDLE)r->proc, 0) != WAIT_OBJECT_0) { return 0; }
		timedOut = 1;
	}
	else if (r->killed) { timedOut = 1; }
	pump_pipe(r);
	GetExitCodeProcess((HANDLE)r->proc, &code);
	release_run(r);
	r->buf[r->n < sizeof(r->buf) ? r->n : sizeof(r->buf) - 1] = 0;
	kvm_hwp_result_parse(res, r->buf, r->n);
	if (res->valid && !timedOut) { *verdict = res->verdict; return 1; }
	*verdict = timedOut ? KVM_HWP_V_TIMEOUT : KVM_HWP_V_CRASH;
	return 1;
}

void kvm_hwtest_abort(kvm_hwtest_run *r)
{
	if (!r->running) { return; }
	TerminateJobObject((HANDLE)r->job, 9);
	release_run(r);
}

// ------------------------------------------------------------------------------------------------------------------------ the process of the test, its own side

static double qpc_ms(void)
{
	LARGE_INTEGER f, t;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t);
	return f.QuadPart > 0 ? (double)t.QuadPart * 1000.0 / (double)f.QuadPart : 0.0;
}

static void put_result(const kvm_hwp_result *r)
{
	char buf[1024];
	const size_t n = kvm_hwp_result_format(r, buf, sizeof(buf));
	HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
	DWORD w = 0;
	if (n > 0 && out != INVALID_HANDLE_VALUE && out != NULL) { WriteFile(out, buf, (DWORD)n, &w, NULL); FlushFileBuffers(out); }
}

// The resolution of the system clock for the test: 1 ms like the session (kvm.c: timeBeginPeriod(1) around the loop that uses the encoder), 0 = the default. winmm.dll is loaded at run time (the self test adds no import).
static int g_clockFine = 0;
static void test_clock(int fine)
{
	typedef unsigned int (WINAPI *tp_fn)(unsigned int);
	HMODULE m = GetModuleHandleW(L"winmm.dll");
	if (m == NULL) { m = mfp_load_system_dll(L"winmm.dll"); }
	if (m == NULL) { return; }
	if (fine && !g_clockFine) { tp_fn f = (tp_fn)GetProcAddress(m, "timeBeginPeriod"); if (f != NULL && f(1) == 0) { g_clockFine = 1; } }
	else if (!fine && g_clockFine) { tp_fn f = (tp_fn)GetProcAddress(m, "timeEndPeriod"); if (f != NULL) { f(1); } g_clockFine = 0; }
}

// -kvmhwtest W H level maxLevel profile luid rev key  ->  the result on stdout; exit code 0 PASS, 1 FAIL, 2 SLOW, 3 wrong arguments
int kvm_hwtest_main(int argc, char **argv)
{
	kvm_hwt_cfg cfg;
	kvm_hwp_result res;
	struct mfh264_err err;
	char key[KVM_HWP_KEY_MAX] = "";
	kvm_mfhw_sel sel;
	int rev;
	memset(&res, 0, sizeof(res));
	if (argc < 10) { return 3; }
	memset(&cfg, 0, sizeof(cfg));
	cfg.codedW = atoi(argv[2]); cfg.codedH = atoi(argv[3]); cfg.level = atoi(argv[4]); cfg.maxLevel = atoi(argv[5]); cfg.profile = atoi(argv[6]);
	cfg.luid = _strtoi64(argv[7], NULL, 10);
	rev = atoi(argv[8]);
	if (cfg.codedW < 64 || cfg.codedH < 64 || cfg.codedW > 8192 || cfg.codedH > 8192 || (cfg.codedW & 15) || (cfg.codedH & 15) || kvm_hwp_size_class((unsigned)cfg.codedW, (unsigned)cfg.codedH) == 0) { return 3; }
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
	cfg.rev = KVM_HW_SELFTEST_REV;
	cfg.nowMs = qpc_ms;
	cfg.clock = test_clock;
	cfg.diag = stderr;						// the sizes of the pictures (the parent sends this stream to NUL; a diagnostics run keeps it)
	res.rev = KVM_HW_SELFTEST_REV;
	if (rev != KVM_HW_SELFTEST_REV) { res.verdict = KVM_HWP_V_FAIL; snprintf(res.rule, sizeof(res.rule), "rev"); res.valid = 1; put_result(&res); return 1; }	// (the parent and the test are of different builds: the executable was replaced meanwhile)
	memset(&err, 0, sizeof(err));
	if (kvm_encbe_mfhw.init(&err) != 0) { res.verdict = KVM_HWP_V_FAIL; snprintf(res.rule, sizeof(res.rule), "init%d", -err.code); res.valid = 1; put_result(&res); return 1; }
	if (!kvm_hwtest_make_key(cfg.luid, (unsigned)cfg.codedW, (unsigned)cfg.codedH, key, sizeof(key), &sel))
	{
		res.verdict = KVM_HWP_V_FAIL; snprintf(res.rule, sizeof(res.rule), "no_hw"); res.valid = 1; put_result(&res);
		kvm_encbe_mfhw.shutdown();
		return 1;
	}
	cfg.key = key;
	kvm_hwt_run(&kvm_encbe_mfhw, &cfg, &res);
	put_result(&res);
	// a diagnostics run passes the key "diag...": the variants of the bit rate (to stderr, after the result, so a hang here costs nothing)
	if (argc > 9 && strncmp(argv[9], "diag", 4) == 0 && strcmp(res.rule, "hang") != 0 && strncmp(res.rule, "open", 4) != 0) { kvm_hwt_diag_clock(&kvm_encbe_mfhw, &cfg, &res); kvm_hwt_diag(&kvm_encbe_mfhw, &cfg); }
	// no kvm_encbe_mfhw.shutdown() after a hang or an exception (the driver may be blocked: the process ends by itself)
	if (res.verdict == KVM_HWP_V_PASS || res.verdict == KVM_HWP_V_SLOW) { kvm_encbe_mfhw.shutdown(); }
	test_clock(0);
	return res.verdict == KVM_HWP_V_PASS ? 0 : (res.verdict == KVM_HWP_V_SLOW ? 2 : 1);
}
