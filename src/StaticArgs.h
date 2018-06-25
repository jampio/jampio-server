#pragma once

#include <jampio/common/CommandArgs.h>
#include <functional>

// TODO: get rid of this
class StaticArgs {
private:
	static std::reference_wrapper<CommandArgs> m_args;
public:
	static void set(CommandArgs& args);
	static CommandArgs& get();
};