#pragma once
#include "Frame.hpp"
#include <queue>
#include <mutex>

class FrameQueue {
private:
	size_t max_capacity;
	std::queue<std::shared_ptr<Frame>> q = {};
	std::mutex mtx;

public:
	FrameQueue(const size_t& s) {
		this->max_capacity = s;
	}

	bool push(std::shared_ptr<Frame> frame) {
		std::lock_guard<std::mutex> lock(mtx);
		if (q.size() >= max_capacity) {
			return false;
		}

		q.push(std::move(frame));
		return true;
	}

	bool emplace(std::shared_ptr<Frame> frame) {
		std::lock_guard<std::mutex> lock(mtx);
		if (q.size() >= max_capacity) {
			return false;
		}

		q.emplace(std::move(frame));
		return true;
	}

	void pop() {
		std::lock_guard<std::mutex> lock(mtx);
		if (!q.empty())
			q.pop();
	}

	std::shared_ptr<Frame> get_front_and_update() {
		std::lock_guard<std::mutex> lock(mtx);
		if (q.empty())
			return {};

		std::shared_ptr<Frame> temp = std::move(q.front());
		q.pop();
		return temp;
	}

	size_t size() {
		std::lock_guard<std::mutex> lock(mtx);
		return q.size();
	}

	bool empty() {
		std::lock_guard<std::mutex> lock(mtx);
		return q.empty();
	}
};