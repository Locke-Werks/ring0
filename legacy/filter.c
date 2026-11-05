#include "filter.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>

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

static int starts_with_ci(const wchar_t *s, const wchar_t *end, const wchar_t *pfx)
{
    while (*pfx) {
        if (s >= end) return 0;
        if (lower_w(*s) != lower_w(*pfx)) return 0;
        ++s; ++pfx;
    }
    return 1;
}

static unsigned parse_u(const wchar_t *s, const wchar_t *end)
{
    unsigned v = 0;
    while (s < end && *s >= L'0' && *s <= L'9') {
        v = v * 10 + (unsigned)(*s - L'0');
        ++s;
    }
    return v;
}

void filter_parse(const wchar_t *in, filter_query_t *q)
{
    q->pid = q->session = 0;
    q->has_pid = q->has_session = 0;
    q->name_len = 0;
    q->name[0] = 0;
    if (!in) return;

    const wchar_t *p = in;
    while (*p) {
        while (*p == L' ') ++p;
        if (!*p) break;

        const wchar_t *start = p;
        while (*p && *p != L' ') ++p;

        if (starts_with_ci(start, p, L"pid:")) {
            q->pid     = parse_u(start + 4, p);
            q->has_pid = 1;
        } else if (starts_with_ci(start, p, L"session:")) {
            q->session     = parse_u(start + 8, p);
            q->has_session = 1;
        } else {
            /* append token + separator space to the fuzzy buffer */
            for (const wchar_t *c = start; c < p && q->name_len < 254; ++c)
                q->name[q->name_len++] = *c;
            if (q->name_len < 254) q->name[q->name_len++] = L' ';
        }
    }
    while (q->name_len > 0 && q->name[q->name_len - 1] == L' ') --q->name_len;
    q->name[q->name_len] = 0;
}

int filter_query_match(const filter_query_t *q, unsigned pid, unsigned session,
                       const wchar_t *name)
{
    if (q->has_pid     && pid     != q->pid)     return 0;
    if (q->has_session && session != q->session) return 0;
    if (q->name_len > 0) {
        if (filter_score(q->name, name) <= 0) return 0;
    }
    return 1;
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

    /* prefix parser */
    filter_query_t q;
    filter_parse(L"chrome pid:1234", &q);
    if (!q.has_pid || q.pid != 1234)                        die();
    if (q.name_len != 6 || q.name[0] != L'c')               die();

    filter_parse(L"session:0", &q);
    if (!q.has_session || q.session != 0)                   die();
    if (q.name_len != 0)                                    die();

    filter_parse(L"", &q);
    if (q.has_pid || q.has_session || q.name_len != 0)      die();
}
#endif
