#include "push2.hpp"

#include <chrono>

#include <libusb.h>
#include <rack.hpp>

namespace push2 {

// --- Screen

bool Screen::begin(NVGcontext* vg) {
	if (!fb_ && !failed_ && !(fb_ = nvgluCreateFramebuffer(vg, kWidth, kHeight, 0))) {
		WARN("Tart: cannot create the %dx%d display framebuffer", kWidth, kHeight);
		failed_ = true;
	}
	if (!fb_)
		return false;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo_);
	glGetIntegerv(GL_VIEWPORT, viewport_);
	glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_);
	glBindFramebuffer(GL_FRAMEBUFFER, fb_->fbo);
	glViewport(0, 0, kWidth, kHeight);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	nvgBeginFrame(vg, kWidth, kHeight, 1);
	return true;
}

void Screen::end(NVGcontext* vg, uint8_t* rgba) {
	nvgEndFrame(vg);
	if (rgba)
		glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
	glViewport(viewport_[0], viewport_[1], viewport_[2], viewport_[3]);
	glClearColor(clear_[0], clear_[1], clear_[2], clear_[3]);
}

void Screen::drawMirror(NVGcontext* vg, float x, float y, float w, float h) const {
	nvgBeginPath(vg);
	nvgRect(vg, x, y, w, h);
	nvgFillColor(vg, nvgRGB(0, 0, 0));
	nvgFill(vg);
	if (!fb_)
		return;
	nvgBeginPath(vg);
	nvgRect(vg, x, y, w, h);
	nvgFillPaint(vg, nvgImagePattern(vg, x, y, w, h, 0.f, fb_->image, 1.f)); // nvglu image is already FLIPY
	nvgFill(vg);
}

void Screen::destroy() {
	if (fb_)
		nvgluDeleteFramebuffer(fb_);
	fb_ = nullptr;
	failed_ = false;
}

// --- Display

namespace {

constexpr unsigned char kEndpoint = 0x01; // "Display Interface Protocol": bulk OUT
constexpr int kChunk = 16384, kTimeoutMs = 1000;

/** Opens the Push 2 and claims its display interface (0); the status says why not. */
Display::Status open(libusb_context* ctx, libusb_device_handle*& h) {
	libusb_device** list;
	ssize_t n = libusb_get_device_list(ctx, &list);
	if (n < 0)
		return Display::Error;
	Display::Status s = Display::NoDevice;
	for (ssize_t i = 0; i < n && s == Display::NoDevice; i++) {
		libusb_device_descriptor d;
		if (libusb_get_device_descriptor(list[i], &d) != 0 || d.idVendor != kVendorId || d.idProduct != kProductId)
			continue;
		int r = libusb_open(list[i], &h);
		if (r == 0 && (r = libusb_claim_interface(h, 0)) != 0) {
			libusb_close(h);
			h = nullptr;
		}
		s = r == 0                            ? Display::Connected
			: r == LIBUSB_ERROR_ACCESS        ? Display::NoAccess // Linux: no udev rule
			: r == LIBUSB_ERROR_NOT_SUPPORTED ? Display::NoDriver // Windows: no WinUSB driver
			: r == LIBUSB_ERROR_BUSY          ? Display::Busy     // Live has it
											  : Display::Error;
	}
	libusb_free_device_list(list, 1);
	return s;
}

/** The header, then the frame in 16 KB bulk transfers. */
bool send(libusb_device_handle* h, const uint8_t* frame) {
	unsigned char header[sizeof kFrameHeader];
	std::memcpy(header, kFrameHeader, sizeof header);
	int sent;
	if (libusb_bulk_transfer(h, kEndpoint, header, sizeof header, &sent, kTimeoutMs) != 0)
		return false;
	for (int i = 0; i < kFrameBytes; i += kChunk)
		if (libusb_bulk_transfer(h, kEndpoint, const_cast<unsigned char*>(frame + i), kChunk, &sent, kTimeoutMs) != 0)
			return false;
	return true;
}

} // namespace

void Display::setStatus(Status s) {
	if (status_.exchange(s) == s)
		return;
	switch (s) {
	case Off:
		break;
	case NoDevice:
		INFO("Push 2 display: no device");
		break;
	case Connected:
		INFO("Push 2 display: connected");
		break;
	case NoAccess:
		WARN("Push 2 display: no access (Linux: needs a udev rule)");
		break;
	case NoDriver:
		WARN("Push 2 display: no WinUSB driver for it (Windows)");
		break;
	case Busy:
		WARN("Push 2 display: in use by another program (Live?)");
		break;
	case Error:
		WARN("Push 2 display: USB error");
		break;
	}
}

void Display::start() {
	if (thread_.joinable())
		return;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		quit_ = fresh_ = false;
		next_.resize((size_t)kWidth * kHeight * 4);
		frame_.resize(kFrameBytes);
	}
	thread_ = std::thread([this] { run(); });
}

void Display::stop() {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		quit_ = true;
	}
	cv_.notify_all();
	if (thread_.joinable())
		thread_.join();
	setStatus(Off);
}

void Display::submit(const uint8_t* rgba) {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (next_.empty()) // not started
			return;
		std::memcpy(next_.data(), rgba, next_.size());
		fresh_ = true;
	}
	cv_.notify_one();
}

void Display::run() {
	using namespace std::chrono_literals;
	libusb_context* ctx = nullptr;
	if (libusb_init(&ctx) != 0) {
		setStatus(Error);
		return;
	}
	libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);
	libusb_device_handle* h = nullptr;
	bool have = false; // a frame has been converted: before that there is nothing to send
	std::unique_lock<std::mutex> lock(mutex_);
	while (!quit_) {
		if (!h) {
			lock.unlock();
			Status s = open(ctx, h);
			lock.lock();
			setStatus(s);
			if (s == Connected)
				connections_.fetch_add(1, std::memory_order_relaxed);
			else
				cv_.wait_for(lock, 1s, [&] { return quit_; }); // retry in 1 s; stop() is prompt
			continue;
		}
		// A fresh frame, or the last one again after 500 ms: the display blanks after 2 s without one.
		cv_.wait_for(lock, 500ms, [&] { return fresh_ || quit_; });
		if (quit_)
			break;
		if (fresh_) {
			// ponytail: converts under the lock (~0.3 ms) so the UI thread's submit() can wait for it; a second buffer
			// swapped under the lock if that ever shows in the frame time.
			toFrame(next_.data(), frame_.data());
			fresh_ = false;
			have = true;
		}
		if (!have)
			continue;
		lock.unlock();
		bool ok = send(h, frame_.data()); // frame_ is this thread's between the unlock and the next wait
		lock.lock();
		if (!ok) {
			libusb_release_interface(h, 0);
			libusb_close(h);
			h = nullptr;
			setStatus(Error);
			cv_.wait_for(lock, 1s, [&] { return quit_; });
		}
	}
	if (h) {
		libusb_release_interface(h, 0);
		libusb_close(h);
	}
	libusb_exit(ctx);
}

} // namespace push2
