#include "fetch.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <thread>
#include <vector>

#include <ghc/filesystem.hpp>

namespace fs = ghc::filesystem;

namespace fetch {

std::string inside(const std::string& root, const std::string& rel) {
	if (root.empty() || rel.empty() || rel[0] == '/' || rel.find_first_of("\\:") != std::string::npos)
		return "";
	for (size_t i = 0; i <= rel.size();) {
		size_t j = std::min(rel.find('/', i), rel.size());
		std::string seg = rel.substr(i, j - i);
		if (seg.empty() || seg == "." || seg == "..")
			return "";
		i = j + 1;
	}
	std::error_code ec;
	fs::path base = fs::weakly_canonical(fs::u8path(root), ec);
	if (ec)
		return "";
	fs::path p = fs::weakly_canonical(base / fs::u8path(rel), ec);
	fs::path r = p.lexically_relative(base);
	if (ec || r.empty() || *r.begin() == ".." || r == ".")
		return "";
	return p.u8string();
}

// ponytail: checks the URL text only. DNS names that resolve to private IPs (rebinding) and HTTP redirects followed by
// Rack's network::requestDownload are not checked; upgrade path: an own curl handle with CURLOPT_REDIR_PROTOCOLS_STR
// "https" and a CURLOPT_OPENSOCKETFUNCTION that checks the peer IP.
std::string remoteUrl(const std::string& url, std::string* why) {
	auto refuse = [&](const char* rule) {
		if (why)
			*why = "refused " + url + ": " + rule;
		return std::string();
	};
	std::string u = url.compare(0, 7, "http://") == 0 ? "https://" + url.substr(7) : url; // like HTTPS-only mode
	if (u.compare(0, 8, "https://") != 0)
		return refuse("only https:// is fetched");
	std::string auth = u.substr(8, u.find_first_of("/?#", 8) - 8);
	if (auth.find('@') != std::string::npos)
		return refuse("no user@ in the host");
	if (auth[0] == '[')
		return refuse("no IPv6 literal");
	std::string host = auth.substr(0, auth.find(':'));
	std::transform(host.begin(), host.end(), host.begin(), [](char c) { return (char)std::tolower((unsigned char)c); });
	auto ends = [&](const char* suffix) {
		size_t n = std::strlen(suffix);
		return host.size() > n && host.compare(host.size() - n, n, suffix) == 0;
	};
	std::string last = host.substr(host.rfind('.') + 1); // npos + 1 = 0: the whole host
	bool letter = std::any_of(last.begin(), last.end(), [](char c) { return std::isalpha((unsigned char)c); });
	if (host.find('.') == std::string::npos || ends(".localhost") || ends(".local") || !letter)
		return refuse("no local or private hosts (localhost, .local, single-label names, IP addresses)");
	return u;
}

std::string cachePath(const std::string& root, const std::string& url) {
	size_t s = url.compare(0, 8, "https://") == 0 ? 8 : url.compare(0, 7, "http://") == 0 ? 7 : 0;
	std::string rest = url.substr(s, url.find('#') - s), query;
	size_t q = rest.find('?');
	if (q != std::string::npos)
		query = rest.substr(q + 1), rest.resize(q);
	size_t slash = rest.find('/');
	if (!s || slash == 0 || slash == std::string::npos || slash + 1 == rest.size() || rest.back() == '/')
		return "";
	std::replace(rest.begin(), rest.begin() + slash, ':', '_'); // host:port
	if (!query.empty())
		rest += "@" + query;
	for (char c : rest)
		if (!std::isalnum((unsigned char)c) && std::string("/._~!$&'()+,;=@%-").find(c) == std::string::npos)
			return "";
	return inside(root, rest);
}

bool download(const Downloader& d, const std::string& url, const std::string& path) {
	static std::atomic<unsigned> serial{0};
	std::error_code ec;
	fs::path p = fs::u8path(path);
	fs::create_directories(p.parent_path(), ec);
	std::string tmp = path + ".part" + std::to_string(serial++);
	bool ok = d(url, tmp);
	std::error_code renamed;
	if (ok)
		fs::rename(fs::u8path(tmp), p, renamed);
	// Windows can't rename over a file another requester has open (a second Filo): that file is the same download.
	if (renamed)
		ok = fs::is_regular_file(p, ec);
	if (!ok || renamed)
		fs::remove(fs::u8path(tmp), ec);
	return ok;
}

uint64_t size(const std::string& dir) {
	std::error_code ec;
	uint64_t n = 0;
	for (fs::recursive_directory_iterator it(fs::u8path(dir), ec), end; !ec && it != end; it.increment(ec))
		if (it->is_regular_file(ec))
			n += it->file_size(ec);
	return n;
}

void clear(const std::string& dir) {
	std::error_code ec;
	fs::remove_all(fs::u8path(dir), ec);
}

struct Worker::State {
	struct Job {
		std::string url, path;
		std::shared_ptr<Inbox> inbox;
		int id;
	};
	Downloader d;
	std::mutex m;
	std::condition_variable cv;
	std::deque<Job> jobs;
	std::vector<std::thread> threads;
	int busy = 0;
	bool stop = false;

	static void run(std::shared_ptr<State> s) {
		std::unique_lock<std::mutex> lock(s->m);
		for (;;) {
			s->cv.wait(lock, [&] { return s->stop || !s->jobs.empty(); });
			if (s->stop)
				return;
			Job j = std::move(s->jobs.front());
			s->jobs.pop_front();
			s->busy++;
			lock.unlock();
			std::error_code ec;
			bool ok = !j.path.empty() && (fs::exists(fs::u8path(j.path), ec) || download(s->d, j.url, j.path));
			j.inbox->post({j.id, ok});
			lock.lock();
			s->busy--;
			s->cv.notify_all();
		}
	}
};

Worker::Worker(std::string root, Downloader d, int threads) : root_(std::move(root)), s_(new State) {
	s_->d = std::move(d);
	s_->threads.resize(threads);
}

Worker::~Worker() {
	std::unique_lock<std::mutex> lock(s_->m);
	s_->stop = true;
	s_->cv.notify_all();
	// ponytail: a blocking download can't be cancelled; past 2 s its thread is detached and may outlive the plugin.
	bool idle = s_->cv.wait_for(lock, std::chrono::seconds(2), [&] { return s_->busy == 0; });
	lock.unlock();
	for (std::thread& t : s_->threads)
		if (t.joinable())
			idle ? t.join() : t.detach();
}

void Worker::request(const std::string& url, std::shared_ptr<Inbox> inbox, int id) {
	std::lock_guard<std::mutex> lock(s_->m);
	for (std::thread& t : s_->threads)
		if (!t.joinable())
			t = std::thread(State::run, s_);
	s_->jobs.push_back({url, cachePath(root_, url), std::move(inbox), id});
	s_->cv.notify_one();
}

} // namespace fetch
