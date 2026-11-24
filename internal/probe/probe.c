/* probe.c - minimal manual-map payload used to bisect the injector path.
 *
 * No CRT init, no imports beyond kernel32, no threads: the export named
 * "DllMain" just appends one line to a world-writable file and returns.
 * If this payload also kills the target, the fault is in the orelia
 * shellcode / OBS hook offset rather than in RobRunInternal.dll.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static char* put_text(char* p, const char* s) { while (*s) *p++ = *s++; return p; }
static char* put_hex(char* p, unsigned long long v) {
    *p++ = '0'; *p++ = 'x';
    int started = 0;
    for (int shift = 60; shift >= 0; shift -= 4) {
        unsigned digit = (unsigned)((v >> shift) & 0xF);
        if (!digit && !started && shift) continue;
        started = 1;
        *p++ = (char)(digit < 10 ? '0' + digit : 'a' + (digit - 10));
    }
    return p;
}
static char* put_dec(char* p, unsigned long v) {
    char tmp[12];
    int n = 0;
    do { tmp[n++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    while (n) *p++ = tmp[--n];
    return p;
}

__declspec(dllexport) BOOL DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    char line[128];
    char* p = put_text(line, "probe rcx=");
    p = put_hex(p, (unsigned long long)(unsigned long long)(ULONG_PTR)inst);
    p = put_text(p, " reason=");
    p = put_dec(p, reason);
    p = put_text(p, " thread=");
    p = put_dec(p, GetCurrentThreadId());
    *p = 0;

    HANDLE f = CreateFileA("C:\\Users\\Public\\rrprobe.txt", FILE_APPEND_DATA,
                           FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD len = 0, wrote = 0;
        while (line[len]) ++len;
        WriteFile(f, line, len, &wrote, NULL);
        WriteFile(f, "\r\n", 2, &wrote, NULL);
        CloseHandle(f);
    }
    return TRUE;
}
