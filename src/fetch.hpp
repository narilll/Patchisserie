#pragma once
// Online samples (plan item 9): a plugin-wide download worker into a disk cache, and the path guards for every local
// file the engine reads. Pure logic: the blocking downloader is injected (the plugin passes
// rack::network::requestDownload, test/test_fetch.cpp a fake), so nothing here needs Rack.
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace fetch {

/** Blocking download of `url` into `file`; false on any failure, HTTP errors included. */
using Downloader = std::function<bool(const std::string& url, const std::string& file)>;

/** root/rel when rel is relative and the result, symlinks resolved, stays inside root; else "". */
std::string inside(const std::string& root, const std::string& rel);

/** Cache file of an http(s) URL: root/<host>/<path>[@<query>]; "" for other URLs or characters outside the URL-safe
set (superdough's sample maps arrive percent-encoded). */
std::string cachePath(const std::string& root, const std::string& url);

/** Downloads into a temp file next to `path`, renamed over it on success. Leaves nothing behind on failure. */
bool download(const Downloader& d, const std::string& url, const std::string& path);

/** Bytes of all files under dir, and removing them (the menu's cache size / clear). */
uint64_t size(const std::string& dir);
void clear(const std::string& dir);

/** Completions for one requester: the worker posts, the requester polls. Shared with the worker, so a requester may
go away while its downloads still run. */
class Inbox {
  public:
	struct Done {
		int id;
		bool ok;
	};
	void post(Done d) {
		std::lock_guard<std::mutex> lock(m_);
		q_.push_back(d);
	}
	bool poll(Done& d) {
		std::lock_guard<std::mutex> lock(m_);
		if (q_.empty())
			return false;
		d = q_.front();
		q_.pop_front();
		return true;
	}

  private:
	std::mutex m_;
	std::deque<Done> q_;
};

/** Download threads, started on the first request. Destruction never waits for a download in flight longer than a
short grace period: such threads are left detached and keep their own state alive. */
class Worker {
  public:
	Worker(std::string root, Downloader d, int threads = 2);
	~Worker();
	Worker(const Worker&) = delete;
	Worker& operator=(const Worker&) = delete;
	const std::string& root() const { return root_; }
	/** Downloads url into cachePath(root(), url) unless that file exists by then; posts {id, ok} to inbox. */
	void request(const std::string& url, std::shared_ptr<Inbox> inbox, int id);

  private:
	struct State;
	std::string root_;
	std::shared_ptr<State> s_;
};

} // namespace fetch
