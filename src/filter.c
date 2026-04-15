#include "filter.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static wchar_t lower_w(wchar_t c)
{
    return (c >= L'A' && c <= L'Z') ? (wchar_t)(c + 32) : c;
}

static int is_boundary(wchar_t c)
{
    return c == L'.' || c == L' ' || c == L'_' || c == L'-' || c == L'\\' || c == L'/';
}

int filter_score(const wchar_t *pat, const wchar_t *hay)
{
    if (!pat || !*pat) return 0;      /* empty pattern matches all */
    if (!hay) return -1;

    int score = 0;
    int pi = 0, hi = 0;
    int prev_boundary = 1;            /* start of haystack counts as boundary */
    int first_match = 1;

    while (pat[pi] && hay[hi]) {
        wchar_t pc = lower_w(pat[pi]);
        wchar_t hc = lower_w(hay[hi]);
        if (pc == hc) {
            score += 10;
            if (prev_boundary) score += 15;
            if (first_match)   { score += 10; first_match = 0; }
            ++pi;
        } else {
            score -= 1;
        }
        prev_boundary = is_boundary(hay[hi]);
        ++hi;
    }

    if (pat[pi] != 0) return -1;      /* not all of pattern matched */
    return score < 1 ? 1 : score;
}

#ifdef DEBUG
static void die(void) { __debugbreak(); ExitProcess(0xF1DEAD); }

void filter_selftest(void)
{
    if (filter_score(L"", L"anything") != 0)               die();
    if (filter_score(L"chrome", L"chrome.exe")   <= 0)      die();
    if (filter_score(L"chr",    L"chrome.exe")   <= 0)      die();
    if (filter_score(L"xyz",    L"chrome.exe")   >= 0)      die();
    if (filter_score(L"rome",   L"chrome.exe")   <= 0)      die();

    /* boundary/start bonus — same pattern should score higher when the first
       match lands at a word boundary vs mid-word. */
    int s_boundary = filter_score(L"chrome", L"chrome.exe");
    int s_midword  = filter_score(L"chrome", L"unchromed.exe");
    if (s_boundary <= s_midword)                            die();

    /* case-insensitive */
    if (filter_score(L"CHROME", L"chrome.exe") <= 0)        die();
    if (filter_score(L"chrome", L"CHROME.EXE") <= 0)        die();
}
#endif
