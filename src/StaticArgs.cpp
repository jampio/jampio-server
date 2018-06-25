#include "StaticArgs.h"

static CommandArgs empty_args = CommandArgs::TokenizeString("");

std::reference_wrapper<CommandArgs> StaticArgs::m_args = empty_args;

void StaticArgs::set(CommandArgs &args) {
	m_args = args;
}

CommandArgs& StaticArgs::get() {
	return m_args;
}