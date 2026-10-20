/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

// The Add dialog's finder: every folder and audio file under the folder
// being browsed, listed by a thread of its own and matched fuzzily against
// what is typed (cb_fuzzy_match), best first.
//
// The walk is breadth first, so the folders near the top (artists,
// albums) are found within a frame or two and the matches come in while
// it goes deeper. It skips hidden entries as the listing does, follows a
// symbolic link to a folder (a folder seen before, by device and inode,
// is not entered again: no loops), stays out of the kernel's own file
// systems (/proc, /sys and the like) when it crosses into one, and stops
// at MAX_ENTRIES entries or MAX_DEPTH folders down, saying so.
//
// The thread hands its entries over in batches under a lock; the
// interface thread moves them into a list of its own, so nothing it holds
// is ever touched by the walker. Matching runs on the interface thread:
// every entry when the query changes, only the last matches when it was
// extended (typing on), the new entries as they arrive, sorted again at
// most every SORT_US while the walk goes on. cb_fuzzy_match rejects an
// ASCII path from its bytes: 300 000 paths take about 0.1 s on this
// machine, a typical music folder of tens of thousands a few ms.

#include "ui_internal.h"

#include <dirent.h>
#include <linux/magic.h>
#include <sys/stat.h>
#include <sys/vfs.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

// A walk of / with these limits takes a few seconds and a few tens of MB.
static const size_t  MAX_ENTRIES = 300000;
static const int     MAX_DEPTH   = 32;
static const size_t  BATCH       = 512;
static const int64_t SORT_US     = 150000;

struct UiFinder {
    std::string root;
    std::thread walker;

    // The walker's side.
    std::mutex lock;
    std::vector<UiFinderEntry> handed;      // under lock
    std::atomic<bool> cancel{ false }, done{ false }, truncated{ false };

    // The interface's side.
    std::vector<UiFinderEntry> entries;
    std::string query;
    size_t matched = 0;                     // entries matched against query so far
    std::vector<std::pair<int, int>> scored;    // (score, entry)
    std::vector<int> results;               // entries, best first
    bool unsorted = false;
    int64_t sorted_at = 0;
};

/* ------------------------------------------------------------------ walk */

// The kernel's file systems, which hold no music and, for /proc, a great
// many entries.
static bool kernel_fs(int fd)
{
    struct statfs sf;
    if (fstatfs(fd, &sf) < 0)
        return false;
    switch ((unsigned long)sf.f_type) {
    case PROC_SUPER_MAGIC:
    case SYSFS_MAGIC:
    case DEVPTS_SUPER_MAGIC:
    case CGROUP_SUPER_MAGIC:
    case CGROUP2_SUPER_MAGIC:
    case DEBUGFS_MAGIC:
    case TRACEFS_MAGIC:
    case SECURITYFS_MAGIC:
    case PSTOREFS_MAGIC:
    case BPF_FS_MAGIC:
        return true;
    default:
        return false;
    }
}

static void walk(UiFinder *f)
{
    cb_thread_name("ui:finder");
    struct Dir {
        std::string rel;
        int   depth;
        dev_t dev;      // of the folder it is in
    };
    std::deque<Dir> queue;
    queue.push_back({ "", 0, 0 });
    std::set<std::pair<dev_t, ino_t>> seen;
    std::vector<UiFinderEntry> batch;
    size_t total = 0;

    auto hand_over = [&]() {
        if (batch.empty())
            return;
        std::lock_guard<std::mutex> g(f->lock);
        for (UiFinderEntry &e : batch)
            f->handed.push_back(std::move(e));
        batch.clear();
    };

    while (!queue.empty() && !f->cancel.load(std::memory_order_relaxed)) {
        Dir d = std::move(queue.front());
        queue.pop_front();
        std::string abs = d.rel.empty() ? f->root
                        : f->root == "/" ? "/" + d.rel : f->root + "/" + d.rel;
        DIR *dp = opendir(abs.c_str());
        if (!dp)
            continue;
        struct stat st;
        if (fstat(dirfd(dp), &st) < 0 || !seen.insert({ st.st_dev, st.st_ino }).second ||
            (!d.rel.empty() && st.st_dev != d.dev && kernel_fs(dirfd(dp)))) {
            closedir(dp);
            continue;
        }
        struct dirent *e;
        while ((e = readdir(dp)) && !f->cancel.load(std::memory_order_relaxed)) {
            if (e->d_name[0] == '.')
                continue;
            bool dir = e->d_type == DT_DIR, file = e->d_type == DT_REG;
            if (e->d_type == DT_LNK || e->d_type == DT_UNKNOWN) {
                struct stat ls;
                if (fstatat(dirfd(dp), e->d_name, &ls, 0) == 0) {
                    dir = S_ISDIR(ls.st_mode);
                    file = S_ISREG(ls.st_mode);
                }
            }
            if (!dir && !(file && cb_is_audio_path(e->d_name)))
                continue;
            std::string rel = d.rel.empty() ? std::string(e->d_name) : d.rel + "/" + e->d_name;
            if (dir) {
                if (d.depth + 1 < MAX_DEPTH)
                    queue.push_back({ rel, d.depth + 1, st.st_dev });
                else
                    f->truncated.store(true, std::memory_order_relaxed);
            }
            batch.push_back({ std::move(rel), dir });
            if (++total >= MAX_ENTRIES) {
                f->truncated.store(true, std::memory_order_relaxed);
                break;
            }
            if (batch.size() >= BATCH)
                hand_over();
        }
        closedir(dp);
        hand_over();
        if (total >= MAX_ENTRIES)
            break;
    }
    hand_over();
    f->done.store(true, std::memory_order_release);
}

/* ---------------------------------------------------------------- matching */

void ui_finder_free(UiFinder **pf)
{
    UiFinder *f = *pf;
    if (!f)
        return;
    f->cancel.store(true, std::memory_order_relaxed);
    if (f->walker.joinable())
        f->walker.join();
    delete f;
    *pf = nullptr;
}

void ui_finder_update(UiFinder **pf, const std::string &root, const char *query)
{
    UiFinder *f = *pf;
    if (f && f->root != root)
        ui_finder_free(pf);
    if (!*pf) {
        f = new UiFinder;
        f->root = root;
        f->walker = std::thread(walk, f);
        *pf = f;
    }

    std::vector<UiFinderEntry> got;
    {
        std::lock_guard<std::mutex> g(f->lock);
        got.swap(f->handed);
    }
    for (UiFinderEntry &e : got)
        f->entries.push_back(std::move(e));

    // A query that extends the last one (a letter typed, a word added)
    // can only match what the last one matched: those are scored again,
    // not every entry. Anything else starts over.
    std::string q = query ? query : "";
    bool fresh = q != f->query;
    if (fresh) {
        bool narrower = !f->query.empty() && q.compare(0, f->query.size(), f->query) == 0;
        std::vector<std::pair<int, int>> kept;
        if (narrower) {
            for (const std::pair<int, int> &m : f->scored) {
                int s = 0;
                if (cb_fuzzy_match(f->entries[(size_t)m.second].rel.c_str(), q.c_str(), &s, nullptr,
                                   0, nullptr))
                    kept.push_back({ s, m.second });
            }
        } else {
            f->matched = 0;
        }
        f->query = q;
        f->scored.swap(kept);
        f->unsorted = true;
    }
    for (; f->matched < f->entries.size(); f->matched++) {
        int s = 0;
        if (cb_fuzzy_match(f->entries[f->matched].rel.c_str(), q.c_str(), &s, nullptr, 0, nullptr)) {
            f->scored.push_back({ s, (int)f->matched });
            f->unsorted = true;
        }
    }

    // The best UI_FINDER_SHOWN, best first (only those are shown: the rest
    // are left as they are); between equals the shorter path (the folder
    // before what is in it), then by name.
    int64_t now = cb_time_us();
    bool walking = !f->done.load(std::memory_order_acquire);
    if (f->unsorted && (fresh || !walking || now - f->sorted_at >= SORT_US)) {
        const std::vector<UiFinderEntry> &en = f->entries;
        size_t k = std::min(f->scored.size(), (size_t)UI_FINDER_SHOWN);
        std::partial_sort(f->scored.begin(), f->scored.begin() + (ptrdiff_t)k, f->scored.end(),
                          [&en](const std::pair<int, int> &a, const std::pair<int, int> &b) {
            if (a.first != b.first)
                return a.first > b.first;
            const std::string &x = en[(size_t)a.second].rel, &y = en[(size_t)b.second].rel;
            if (x.size() != y.size())
                return x.size() < y.size();
            return x < y;
        });
        f->results.clear();
        for (size_t i = 0; i < k; i++)
            f->results.push_back(f->scored[i].second);
        f->unsorted = false;
        f->sorted_at = now;
    }
}

const std::vector<int> &ui_finder_results(const UiFinder *f)
{
    return f->results;
}

size_t ui_finder_matches(const UiFinder *f)
{
    return f->scored.size();
}

const UiFinderEntry &ui_finder_entry(const UiFinder *f, int i)
{
    return f->entries[(size_t)i];
}

size_t ui_finder_count(const UiFinder *f)
{
    return f->entries.size();
}

bool ui_finder_walking(const UiFinder *f)
{
    return !f->done.load(std::memory_order_acquire);
}

bool ui_finder_truncated(const UiFinder *f)
{
    return f->truncated.load(std::memory_order_relaxed);
}
