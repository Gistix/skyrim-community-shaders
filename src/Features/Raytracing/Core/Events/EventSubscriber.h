#pragma once

#include "PCH.h"

#include "Features/Raytracing/Core/Events/EventBus.h"

template <typename EventType>
class EventSubscriber :
	public eastl::enable_shared_from_this<EventSubscriber<EventType>>
{
public:
	virtual ~EventSubscriber() = default;

	void Subscribe(EventBus<EventType>& bus)
	{
		bus.Subscribe(this->weak_from_this(),
			[this](const EventType& e) {
				OnEvent(e);
			});
	}

protected:
	virtual void OnEvent(const EventType& e) = 0;
};