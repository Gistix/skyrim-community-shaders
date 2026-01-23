#pragma once

#include "PCH.h"

#include "Raytracing/Core/Events/UpdateDismemberPartionEvent.h"

class UpdateDismemberPartionSource :
	public RE::BSTEventSource<UpdateDismemberPartionEvent>
{
public:
	static UpdateDismemberPartionSource* GetSingleton()
	{
		static UpdateDismemberPartionSource instance;
		return std::addressof(instance);
	}

	void Send(const UpdateDismemberPartionEvent& e)
	{
		Notify(e);
	}
};