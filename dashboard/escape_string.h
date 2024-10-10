/*
 * WiVRn VR streaming
 * Copyright (C) 2024  Guillaume Meunier <guillaume.meunier@centraliens.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <string>
#include <vector>

constexpr std::string escape_string(const std::vector<std::string> & app)
{
	constexpr std::string_view escaped_chars = R"( '"\)";
	std::string app_string;
	for (const auto & i: app)
	{
		for (char c: i)
		{
			if (escaped_chars.find(c) == std::string_view::npos)
			{
				app_string += c;
			}
			else
			{
				app_string += '\\';
				app_string += c;
			}
		}

		app_string += ' ';
	}
	app_string.resize(app_string.size() - 1);

	return app_string;
}

constexpr std::vector<std::string> unescape_string(const std::string & app_string)
{
	std::vector<std::string> app;
	app.emplace_back();

	bool seen_backslash = false;
	bool seen_single_quote = false;
	bool seen_double_quote = false;
	for (auto c: app_string)
	{
		if (seen_backslash)
		{
			app.back() += c;
			seen_backslash = false;
		}
		else if (seen_single_quote)
		{
			if (c == '\'')
				seen_single_quote = false;
			else if (c == '\\')
				seen_backslash = true;
			else
				app.back() += c;
		}
		else if (seen_double_quote)
		{
			if (c == '"')
				seen_double_quote = false;
			else if (c == '\\')
				seen_backslash = true;
			else
				app.back() += c;
		}
		else
		{
			switch (c)
			{
				case '\\':
					seen_backslash = true;
					break;
				case '\'':
					seen_single_quote = true;
					break;
				case '"':
					seen_double_quote = true;
					break;
				case ' ':
					if (not app.back().empty())
						app.emplace_back();
					break;
				default:
					app.back() += c;
			}
		}
	}

	if (app.back().empty())
		app.pop_back();

	return app;
}

#ifdef __clang__
static_assert(escape_string({"abc", "def"}) == R"(abc def)");
static_assert(escape_string({"ab\"c", "def"}) == R"(ab\"c def)");
static_assert(escape_string({"ab'c", "def"}) == R"(ab\'c def)");
static_assert(escape_string({"ab\\c", "def"}) == R"(ab\\c def)");

static_assert(unescape_string(R"(abc def)") == std::vector<std::string>{"abc", "def"});
static_assert(unescape_string(R"(ab\"c def)") == std::vector<std::string>{"ab\"c", "def"});
static_assert(unescape_string(R"(ab\'c def)") == std::vector<std::string>{"ab'c", "def"});
static_assert(unescape_string(R"(ab\\c def)") == std::vector<std::string>{"ab\\c", "def"});
#endif