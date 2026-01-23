#pragma once

#include "PCH.h"

#include <functional>
#include <vector>
#include <memory>

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

template <typename EventType>
class EventBus
{
public:
	using Callback = std::function<void(const EventType&)>;

	// Subscribe with a weak_ptr to the owning object
	template <typename T>
	void Subscribe(std::weak_ptr<T> owner, Callback cb)
	{
		listeners.push_back({ owner, std::move(cb) });
	}

	// Emit event to all alive listeners
	void Emit(const EventType& e)
	{
		// Remove expired listeners
		listeners.erase(std::remove_if(listeners.begin(), listeners.end(),
							[](const Listener& l) { return l.owner.expired(); }),
			listeners.end());

		// Notify alive listeners
		for (auto& l : listeners) {
			if (!l.owner.expired()) {
				l.cb(e);
			}
		}
	}

private:
	struct Listener
	{
		std::weak_ptr<void> owner;  // weak_ptr for lifetime safety
		Callback cb;
	};

	std::vector<Listener> listeners;
};