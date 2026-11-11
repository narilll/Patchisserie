// fetch.hpp: cache paths, path guards, temp + rename, the worker's completion queue (fake downloader, no network).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>

#include <ghc/filesystem.hpp>

#include "check.hpp"
#include "fetch.hpp"

namespace fs = ghc::filesystem;

namespace {

std::string tempDir(const char* name) {
	fs::path p = fs::temp_directory_path() / ("patchisserie-" + std::string(name));
	fs::remove_all(p);
	fs::create_directories(p);
	return fs::weakly_canonical(p).u8string();
}

void write(const std::string& path, const std::string& bytes) {
	std::ofstream(path, std::ios::binary) << bytes;
}

std::string read(const std::string& path) {
	std::ifstream f(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(f), {});
}

int files(const std::string& dir) {
	int n = 0;
	for (auto& e : fs::recursive_directory_iterator(dir))
		n += e.is_regular_file();
	return n;
}

} // namespace

TEST(fetch_cache_path) {
	std::string root = tempDir("cache");
	CHECK(fetch::cachePath(root, "https://strudel.b-cdn.net/piano/A0v8.mp3") ==
		  root + "/strudel.b-cdn.net/piano/A0v8.mp3");
	CHECK(fetch::cachePath(root, "http://h:8080/a/b.wav?x=1&y=2#frag") == root + "/h_8080/a/b.wav@x=1&y=2");
	CHECK(fetch::cachePath(root, "https://h/VCSL/Edge-blown%20Aerophones/w.wav") ==
		  root + "/h/VCSL/Edge-blown%20Aerophones/w.wav");
	for (const char* bad : {"ftp://h/a", "file:///etc/passwd", "https://h", "https://h/", "https:///a", "https://h/a/",
							"https://h/../../etc/passwd", "https://h/a/./b", "https://h/a//b", "https://h/a b",
							"https://h/a\\b", "https://h/C:/x", "https://h/\xc3\xa9", "patchisserie:engine.js"})
		CHECK(fetch::cachePath(root, bad).empty());
}

TEST(fetch_inside_guard) {
	std::string root = tempDir("inside");
	fs::create_directories(root + "/kick");
	write(root + "/kick/a b.wav", "x");
	CHECK(fetch::inside(root, "kick/a b.wav") == root + "/kick/a b.wav");
	CHECK(fetch::inside(root, "new/file.wav") == root + "/new/file.wav"); // need not exist yet
	for (const char* bad : {"", "/etc/passwd", "../x", "kick/../../x", "kick/./a b.wav", "kick//a", "a\\b", "C:x"})
		CHECK(fetch::inside(root, bad).empty());
	CHECK(fetch::inside("", "kick/a b.wav").empty());
	// A symlink inside the folder that points out of it is resolved and refused.
	std::string outside = tempDir("outside");
	write(outside + "/secret", "s");
	fs::create_directory_symlink(outside, root + "/link");
	CHECK(fetch::inside(root, "link/secret").empty());
}

TEST(fetch_download_temp_rename) {
	std::string root = tempDir("download");
	std::string path = fetch::cachePath(root, "https://h/a/b.wav");
	std::string seen;
	auto ok = [&](const std::string& url, const std::string& file) {
		seen = file;
		CHECK(url == "https://h/a/b.wav");
		write(file, "data");
		return true;
	};
	CHECK(fetch::download(ok, "https://h/a/b.wav", path));
	CHECK(seen != path && seen.find(path) == 0); // written next to it, then renamed
	CHECK(read(path) == "data" && files(root) == 1);
	// A failed download (HTTP error, partial body) leaves no file, and an existing one untouched.
	auto fail = [](const std::string&, const std::string& file) {
		write(file, "<html>404</html>");
		return false;
	};
	CHECK(!fetch::download(fail, "https://h/a/c.wav", fetch::cachePath(root, "https://h/a/c.wav")));
	CHECK(!fetch::download(fail, "https://h/a/b.wav", path));
	CHECK(read(path) == "data" && files(root) == 1);
	CHECK(fetch::size(root) == 4);
	fetch::clear(root);
	CHECK(!fs::exists(root) && fetch::size(root) == 0);
}

TEST(fetch_worker_completions) {
	std::string root = tempDir("worker");
	std::atomic<int> calls{0};
	auto inbox = std::make_shared<fetch::Inbox>();
	{
		fetch::Worker w(root, [&](const std::string& url, const std::string& file) {
			calls++;
			if (url.find("missing") != std::string::npos)
				return false;
			write(file, url);
			return true;
		});
		w.request("https://h/one.wav", inbox, 1);
		w.request("https://h/missing.wav", inbox, 2);
		w.request("not a url", inbox, 3);
		std::vector<fetch::Inbox::Done> done;
		for (int i = 0; i < 2000 && done.size() < 3; i++) {
			fetch::Inbox::Done d;
			while (inbox->poll(d))
				done.push_back(d);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		CHECK(done.size() == 3);
		for (auto& d : done)
			CHECK(d.ok == (d.id == 1));
		CHECK(read(root + "/h/one.wav") == "https://h/one.wav");
		CHECK(!fs::exists(root + "/h/missing.wav"));
		// Cached by now: a second request doesn't download again.
		w.request("https://h/one.wav", inbox, 4);
		fetch::Inbox::Done d{0, false};
		for (int i = 0; i < 2000 && !inbox->poll(d); i++)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		CHECK(d.id == 4 && d.ok && calls == 2);
	}
	// The requester outlives nothing: posting into an inbox whose owner is gone is fine (shared), and the worker's
	// destructor returned although a request may still have been queued.
	fetch::Worker slow(root, [](const std::string&, const std::string&) {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		return false;
	});
	slow.request("https://h/slow.wav", std::make_shared<fetch::Inbox>(), 5);
}
