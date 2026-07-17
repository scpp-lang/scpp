module;

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

export module scpp.lexer;

#define SCPP_LEXER_MODULE_EXPORT
#include "lexer.h"
#undef SCPP_LEXER_MODULE_EXPORT
