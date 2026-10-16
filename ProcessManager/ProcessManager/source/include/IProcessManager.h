#pragma once
#include <legacy/common/type/IObject.h>
namespace process_manager {

	class IProcessManager : public legacy::type::IObject {
	public:
		virtual int StartUp() = 0;
		virtual int TearDown() = 0;
		virtual void Run() = 0;
		virtual void RequestStop() = 0; // thread-safe: Run() stops the services and returns

		virtual ~IProcessManager() = default;
	};
}