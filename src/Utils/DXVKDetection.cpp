#include "DXVKDetection.h"

namespace Util
{
	namespace DXVK
	{
		static bool IsDXVKModule(const wchar_t* a_moduleName)
		{
			auto module = GetModuleHandleW(a_moduleName);
			if (!module)
				return false;

			wchar_t path[MAX_PATH];
			auto len = GetModuleFileNameW(module, path, MAX_PATH);
			if (!len)
				return false;

			// Check if the DLL is in the system directory
			// If not, it's a proxy DLL (DXVK or similar)

			wchar_t systemDir[MAX_PATH];
			auto sysLen = GetSystemDirectoryW(systemDir, MAX_PATH);
			if (!sysLen)
				return true;

			// Compare case-insensitively with system directory prefix
			return _wcsnicmp(path, systemDir, sysLen) != 0;
		}

		bool IsRunning()
		{
			static bool s_Result = []()
			{
				return IsDXVKModule(L"d3d11.dll") || IsDXVKModule(L"dxgi.dll");
			}();

			return s_Result;
		}
	}
}
