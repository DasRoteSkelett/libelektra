#include <utility>

/**
 * @file
 *
 * @brief
 *
 * @copyright BSD License (see LICENSE.md or https://www.libelektra.org)
 */

#include "elektragen.hpp"

#include <command.hpp>
#include <kdbhelper.h>
#include <kdbtypes.h>
#include <modules.hpp>

#include <fstream>
#include <kdb.h>
#include <kdbease.h>
#include <memory>
#include <regex>
#include <set>
#include <streambuf>
#include <string>

const char * ElektraGenTemplate::Params::InitFunctionName = "initFn";
const char * ElektraGenTemplate::Params::HelpFunctionName = "helpFn";
const char * ElektraGenTemplate::Params::SpecloadFunctionName = "specloadFn";
const char * ElektraGenTemplate::Params::EnumConversion = "enumConv";
const char * ElektraGenTemplate::Params::AdditionalHeaders = "headers";

enum class EnumConversion
{
	Default,
	Trie,
	Strcmp
};

class EnumTrie
{
public:
	explicit EnumTrie (const std::set<std::pair<std::string, std::string>> & values);

	EnumTrie () : children (), stringValue (), name ()
	{
	}

	std::string createSwitch ();

	size_t getDepth ();

private:
	std::map<char, std::unique_ptr<EnumTrie>> children;
	std::string stringValue;
	std::string name;

	void insert (const std::string & prefix, const std::set<std::pair<std::string, std::string>> & values);
	bool createSwitch (std::stringstream & ss, size_t index);
};

class EnumProcessor
{
private:
	std::unordered_map<std::string, std::pair<std::string, std::string>> enumTypes;

	static kainjow::mustache::list getValues (const std::string & prefix, const kdb::Key & key, std::string & fromStringSwitch,
						  std::string & valuesString, size_t & trieDepth);

	static inline bool shouldGenerateTypeDef (const kdb::Key & key);

	EnumConversion conversion;

public:
	explicit EnumProcessor (EnumConversion conversion_) : conversion (conversion_)
	{
	}

	kainjow::mustache::object process (const kdb::Key & key, const std::string & tagName);
	static inline std::string getType (const kdb::Key & key, const std::string & tagName, bool & genType);
};

class StructProcessor
{
private:
	std::unordered_map<std::string, std::pair<std::string, std::string>> structTypes;
	const kdb::Key & parentKey;
	const kdb::KeySet & allKeys;

	kainjow::mustache::list getFields (const kdb::Key & structKey, const kdb::KeySet & structKeys, bool allocating,
					   const std::string & tagName, size_t & maxFieldNameLen, std::string & fieldsString);

	static inline std::string getFieldName (const kdb::Key & key, const std::string & fieldKeyName);
	static inline bool shouldGenerateTypeDef (const kdb::Key & key);
	static inline std::string arraySizeName (const kdb::Key & key, const std::string & arrayFieldName);

public:
	StructProcessor (const kdb::Key & parentKey_, const kdb::KeySet & allKeys_) : parentKey (parentKey_), allKeys (allKeys_)
	{
	}

	static inline bool shouldAllocate (const kdb::Key & key);
	static inline std::string getType (const kdb::Key & key, const std::string & tagName, bool & genType);

	kainjow::mustache::object process (const kdb::Key & key, const kdb::KeySet & subkeys, const std::string & tagName);
};

static void escapeNonAlphaNum (std::string & str)
{
	std::replace_if (str.begin (), str.end (), std::not1 (std::ptr_fun (isalnum)), '_');
}

static std::string createIncludeGuard (const std::string & fileName)
{
	std::string result;
	result.resize (fileName.length ());
	std::transform (fileName.begin (), fileName.end (), result.begin (), ::toupper);
	escapeNonAlphaNum (result);
	return result;
}

static inline bool hasType (const kdb::Key & key)
{
	return key.hasMeta ("type") && !key.getMeta<std::string> ("type").empty ();
}

static inline std::string getType (const kdb::Key & key)
{
	return key.getMeta<std::string> ("type");
}

static std::string getTagName (const kdb::Key & key, const std::string & parentKey)
{
	auto name = key.getName ();
	name.erase (0, parentKey.length () + 1);

	name = std::regex_replace (name, std::regex ("/[#_]/"), "/");
	name = std::regex_replace (name, std::regex ("[#_]/"), "/");
	name = std::regex_replace (name, std::regex ("/[#_]"), "/");
	name = std::regex_replace (name, std::regex (R"(/%(([^/]|(\\\\)*\\/|(\\\\)+)+)%/)"), "/$1/");

	if (name[name.length () - 1] == '/')
	{
		name.erase (name.length () - 1);
	}

	std::replace_if (name.begin (), name.end (), std::not1 (std::ptr_fun (isalnum)), '_');

	return name;
}

static std::string snakeCaseToCamelCase (const std::string & s, bool upper = false)
{
	std::string result;
	result.resize (s.size ());
	auto upcase = upper;
	std::transform (s.begin (), s.end (), result.begin (), [&upcase](char c) {
		int x = upcase ? toupper (c) : c;
		upcase = c == '_';
		return x;
	});
	result.erase (std::remove (result.begin (), result.end (), '_'), result.end ());
	return result;
}

static inline std::string snakeCaseToPascalCase (const std::string & s)
{
	return snakeCaseToCamelCase (s, true);
}

static std::string snakeCaseToMacroCase (const std::string & s)
{
	std::string result;
	result.resize (s.size ());
	std::transform (s.begin (), s.end (), result.begin (), ::toupper);
	return result;
}

static std::string camelCaseToMacroCase (const std::string & s)
{
	std::stringstream ss;
	std::for_each (s.begin (), s.end (), [&ss](char c) {
		if (ss.tellp () != std::stringstream::beg && isupper (c))
		{
			ss << '_';
		}
		ss << static_cast<char> (toupper (c));
	});
	return ss.str ();
}

static void printWrapped (std::ostream & out, std::string line, size_t maxChars)
{
	// find indent
	std::stringstream indent;
	for (auto cur = line.begin (); cur != line.end () && isspace (*cur); ++cur)
	{
		indent << *cur;
	}

	if (indent.str ().length () == line.length ())
	{
		// only whitespace -> skip
		out << std::endl;
		return;
	}

	auto indentSize = 0;

	// wrap at about 'maxChars' chars, keeping indent
	while (line.length () > maxChars)
	{
		// find last space in chunk (outside of quotes)
		size_t lastSpace = 0;
		char quote = '\0';
		for (size_t i = 0; i < maxChars - indentSize; ++i)
		{
			if (quote != '\0')
			{
				// inside quotes -> look for end
				if (line[i - 1] != '\\' && line[i] == quote)
				{
					quote = '\0';
				}
			}
			else if (isspace (line[i]))
			{
				// space outside quotes
				lastSpace = i;
			}
			else if (line[i] == '\'' || line[i] == '"')
			{
				// start of quote
				quote = line[i];
			}
		}

		if (lastSpace > 0)
		{
			// replace space with newline
			out << line.substr (0, lastSpace) << std::endl << indent.str ();
			line.erase (0, lastSpace + 1);
			indentSize = indent.str ().length ();
		}
		else
		{
			// force wrap
			out << line.substr (0, maxChars) << "\\" << std::endl;
			line.erase (0, maxChars);
			indentSize = 0;
		}
	}
	out << line << std::endl;
}

static std::string keySetToCCode (kdb::KeySet set)
{
	using namespace kdb;
	using namespace kdb::tools;

	Modules modules;
	PluginPtr plugin = modules.load ("c", KeySet ());

	auto file = "/tmp/elektra.elektragen." + std::to_string (std::time (nullptr));
	Key errorKey ("", KEY_VALUE, file.c_str (), KEY_END);
	plugin->set (set, errorKey);

	std::ifstream is (file);
	std::string line;

	std::stringstream ss;
	while (std::getline (is, line))
	{
		printWrapped (ss, line, 120);
	}

	return ss.str ();
}

kainjow::mustache::list EnumProcessor::getValues (const std::string & prefix, const kdb::Key & key, std::string & fromStringSwitch,
						  std::string & valuesString, size_t & trieDepth)
{
	using namespace kainjow::mustache;

	if (!key.hasMeta ("check/enum"))
	{
		return {};
	}

	list values;
	std::stringstream ss;
	std::set<std::pair<std::string, std::string>> stringValues;

	const auto end = key.getMeta<std::string> ("check/enum");
	kdb::long_long_t i = 0;
	std::string cur = "#0";
	while (cur <= end)
	{
		if (key.hasMeta ("check/enum/" + cur))
		{
			auto name = prefix + "_";
			const std::string & stringValue = key.getMeta<std::string> ("check/enum/" + cur);
			name += camelCaseToMacroCase (stringValue);
			escapeNonAlphaNum (name);
			auto value = std::to_string (i);
			if (key.hasMeta ("check/enum/" + cur + "/value"))
			{
				value = key.getMeta<std::string> ("gen/enum/" + cur + "/value");
			}
			values.emplace_back (object{ { "name", name }, { "value", value }, { "string_value", stringValue } });
			stringValues.insert ({ stringValue, name });
			ss << name << "=" << value << "\n";
		}
		++i;
		auto indexString = std::to_string (i);
		cur = "#" + std::string (indexString.length () - 1, '_') + std::to_string (i);
	}

	EnumTrie trie (stringValues);
	trieDepth = trie.getDepth ();
	fromStringSwitch = trie.createSwitch ();

	valuesString = ss.str ();

	return values;
}

std::string EnumProcessor::getType (const kdb::Key & key, const std::string & tagName, bool & genType)
{
	genType = key.hasMeta ("gen/enum/type");
	return genType ? key.getMeta<std::string> ("gen/enum/type") : snakeCaseToPascalCase (tagName);
}

bool EnumProcessor::shouldGenerateTypeDef (const kdb::Key & key)
{
	return !key.hasMeta ("gen/enum/create") || key.getMeta<std::string> ("gen/enum/create") == "1";
}

kainjow::mustache::object EnumProcessor::process (const kdb::Key & key, const std::string & tagName)
{
	using namespace kainjow::mustache;

	auto name = key.getName ();
	name.erase (0, sizeof ("spec") - 1);

	bool genType;
	auto enumType = getType (key, tagName, genType);
	auto typeName = "Enum" + enumType;

	auto nativeType = genType ? enumType : "Elektra" + typeName;
	std::string fromStringSwitch;

	std::string valuesString;
	size_t trieDepth;
	auto values = getValues (camelCaseToMacroCase (nativeType), key, fromStringSwitch, valuesString, trieDepth);

	auto isNew = true;
	auto generateTypeDef = shouldGenerateTypeDef (key);
	if (genType && generateTypeDef)
	{
		auto other = enumTypes.find (typeName);
		if (other != enumTypes.end ())
		{
			auto otherValuesString = other->second.second;
			if (otherValuesString != valuesString)
			{
				auto otherKey = other->second.first;
				auto msg = "The key '" + name;
				msg += "' uses the same 'gen/enum/type' as the key '" + otherKey +
				       "', but their 'check/enum' values are different!";
				throw CommandAbortException (msg);
			}

			isNew = false;
		}

		enumTypes[typeName] = std::make_pair (name, valuesString);
	}

	auto useTrie = conversion == EnumConversion::Trie || (conversion == EnumConversion::Default && trieDepth < 3);

	return object{ { "new", isNew },
		       { "name", name },
		       { "tag_name", snakeCaseToMacroCase (tagName) },
		       { "type_name", typeName },
		       { "native_type", nativeType },
		       { "generate_typedef?", generateTypeDef },
		       { "values", values },
		       { "switch_from_string?", useTrie },
		       { "from_string_code", fromStringSwitch } };
}

std::string StructProcessor::getType (const kdb::Key & key, const std::string & tagName, bool & genType)
{
	genType = key.hasMeta ("gen/struct/type");
	return genType ? key.getMeta<std::string> ("gen/struct/type") : snakeCaseToPascalCase (tagName);
}

bool StructProcessor::shouldGenerateTypeDef (const kdb::Key & key)
{
	return !key.hasMeta ("gen/struct/create") || key.getMeta<std::string> ("gen/struct/create") == "1";
}

bool StructProcessor::shouldAllocate (const kdb::Key & key)
{
	return key.hasMeta ("gen/struct/alloc") && key.getMeta<std::string> ("gen/struct/alloc") == "1";
}

std::string StructProcessor::arraySizeName (const kdb::Key & key, const std::string & arrayFieldName)
{
	return key.hasMeta ("gen/struct/array/sizefield") ? key.getMeta<std::string> ("gen/struct/array/sizefield") :
							    arrayFieldName + "Size";
}

std::string StructProcessor::getFieldName (const kdb::Key & key, const std::string & fieldKeyName)
{
	std::string result = key.hasMeta ("gen/struct/field") ? key.getMeta<std::string> ("gen/struct/field") : fieldKeyName;
	escapeNonAlphaNum (result);
	return result;
}

static void processStructRef (const kdb::Key & key, const kdb::Key & parentKey, const kdb::KeySet & allKeys, const std::string & tagName,
			      std::string & typeName, std::string & nativeType, bool & alloc)
{
	if (!key.hasMeta ("check/reference/restrict"))
	{
		throw CommandAbortException ("Keys with type struct_ref must also define 'check/reference/restrict'. Key: '" +
					     key.getName () + "'.");
	}

	auto restrict = key.getMeta<std::string> ("check/reference/restrict");
	char * rawResolved = ckdb::elektraResolveReference (restrict.c_str (), key.getKey (), parentKey.getKey ());

	auto restrictKey = allKeys.lookup (rawResolved);
	ckdb::elektraFree (rawResolved);

	if (!restrictKey)
	{
		throw CommandAbortException ("'check/reference/restrict' of key '" + key.getName () + "' resolves to an unspecified key.");
	}

	if (restrictKey.getMeta<std::string> ("type") != "struct")
	{
		throw CommandAbortException ("'check/reference/restrict' of key '" + key.getName () + "' resolves to a non-struct key.");
	}

	bool genType;
	auto structType = StructProcessor::getType (restrictKey, tagName, genType);
	typeName = "Struct" + structType;
	nativeType = genType ? structType : "Elektra" + typeName;
	alloc = StructProcessor::shouldAllocate (restrictKey);
}

static std::vector<std::string> getKeyParts (const kdb::Key & key)
{
	auto rawName = static_cast<const char *> (ckdb::keyUnescapedName (key.getKey ()));
	size_t size = ckdb::keyGetUnescapedNameSize (key.getKey ());
	std::vector<char> name (rawName, rawName + size);
	auto cur = name.begin ();
	std::vector<std::string> parts;
	while (cur != name.end ())
	{
		auto next = std::find (cur, name.end (), '\0');
		parts.emplace_back (cur, next);
		cur = next + 1;
	}
	return parts;
}

kainjow::mustache::list StructProcessor::getFields (const kdb::Key & structKey, const kdb::KeySet & structKeys, bool allocating,
						    const std::string & tagName, size_t & maxFieldNameLen, std::string & fieldsString)
{
	using namespace kainjow::mustache;

	list fields;
	std::stringstream ss;

	size_t baseParts = getKeyParts (structKey).size ();

	maxFieldNameLen = 0;
	for (const kdb::Key & key : structKeys)
	{
		auto parts = getKeyParts (key);
		auto isArray = parts.back () == "#";

		auto end = isArray ? parts.end () - 1 : parts.end ();
		std::string fieldKeyName = parts[baseParts];
		for (auto it = parts.begin () + baseParts + 1; it != end; ++it)
		{
			fieldKeyName += "_" + *it;
		}
		fieldKeyName = snakeCaseToCamelCase (fieldKeyName);

		const std::string & type = ::getType (key);

		std::unordered_set<std::string> allowedTypes = { "struct_ref", "enum",		"string",     "boolean",
								 "char",       "octet",		"short",      "unsigned_short",
								 "long",       "unsigned_long", "long_long",  "unsigned_long_long",
								 "float",      "double",	"long_double" };

		if (type == "struct")
		{
			auto msg = "The key '" + key.getName ();
			msg += "' has an unsupported type ('" + type + "')! Cannot have structs inside structs, please use struct_ref.";
			throw CommandAbortException (msg);
		}

		if (allowedTypes.find (type) == allowedTypes.end ())
		{
			auto msg = "The key '" + key.getName ();
			msg += "' has an unsupported type ('" + type + "')!";
			throw CommandAbortException (msg);
		}

		auto isStruct = type == "struct_ref";

		if (!allocating && isStruct)
		{
			auto msg = "Cannot have struct_refs inside non-allocating structs. The key '" + key.getName ();
			msg += "' is a struct_ref appearing inside '" + structKey.getName () + ", which is a non-allocating struct.";
			throw CommandAbortException (msg);
		}

		if (!allocating && isArray)
		{
			auto msg = "Cannot have arrays inside non-allocating structs. The key '" + key.getName ();
			msg += "' is an array appearing inside '" + structKey.getName () + ", which is a non-allocating struct.";
			throw CommandAbortException (msg);
		}

		auto typeName = snakeCaseToPascalCase (type);
		auto nativeType = type == "string" ? "const char *" : "kdb_" + type + "_t";

		if (type == "enum")
		{
			bool genType;
			auto enumType = EnumProcessor::getType (key, tagName, genType);

			typeName = "Enum" + enumType;
			nativeType = genType ? enumType : "Elektra" + typeName;
		}

		bool allocate;
		if (isStruct)
		{
			processStructRef (key, parentKey, allKeys, tagName, typeName, nativeType, allocate);
		}
		else
		{
			allocate = true;
		}

		auto name = getFieldName (key, fieldKeyName);

		auto keyName = key.getName ().substr (structKey.getName ().length () + 1);
		maxFieldNameLen = std::max (maxFieldNameLen, keyName.size ());

		if (isArray)
		{
			keyName = keyName.substr (0, keyName.length () - 2);
		}

		auto field =
			object{ { "name", name },       { "key_name", keyName },  { "native_type", nativeType }, { "type_name", typeName },
				{ "alloc?", allocate }, { "is_array?", isArray }, { "is_struct?", isStruct } };

		if (isArray)
		{
			auto sizeName = arraySizeName (key, fieldKeyName);

			fields.emplace_back (object{ { "name", sizeName },
						     { "is_array_size?", true },
						     { "array_key", fieldKeyName },
						     { "native_type", "kdb_long_long_t" } });

			field["size_field"] = sizeName;
		}

		fields.emplace_back (field);

		ss << nativeType << " " << name << "\n";
	}

	fieldsString = ss.str ();

	return fields;
}

kainjow::mustache::object StructProcessor::process (const kdb::Key & key, const kdb::KeySet & subkeys, const std::string & tagName)
{
	using namespace kainjow::mustache;

	auto name = key.getName ();
	name.erase (0, sizeof ("spec") - 1);

	bool genType;
	auto structType = getType (key, tagName, genType);
	auto typeName = "Struct" + structType;

	auto nativeType = genType ? structType : "Elektra" + typeName;

	std::string fieldsString;
	size_t maxFieldNameLen;

	auto allocate = shouldAllocate (key);
	auto fields = getFields (key, subkeys, allocate, tagName, maxFieldNameLen, fieldsString);

	auto isNew = true;
	auto generateTypeDef = shouldGenerateTypeDef (key);
	if (genType && generateTypeDef)
	{
		auto other = structTypes.find (typeName);
		if (other != structTypes.end ())
		{
			auto otherFieldsString = other->second.second;
			if (otherFieldsString != fieldsString)
			{
				auto otherKey = other->second.first;
				auto msg = "The key '" + name;
				msg += "' uses the same 'gen/struct/type' as the key '" + otherKey + "', but their fields are different!";
				throw CommandAbortException (msg);
			}

			isNew = false;
		}

		structTypes[typeName] = std::make_pair (name, fieldsString);
	}

	return object{ { "new", isNew },
		       { "type_name", typeName },
		       { "native_type", nativeType },
		       { "generate_typedef?", generateTypeDef },
		       { "fields", fields },
		       { "max_field_len", std::to_string (maxFieldNameLen + 1) },
		       { "alloc?", allocate } };
}

static inline std::string getArgName (const kdb::Key & key, kdb_long_long_t index, const std::string & defaultPrefix)
{
	auto indexStr = std::to_string (index);
	auto metaName = "gen/arg/name/#" + std::string (indexStr.length () - 1, '_') + indexStr;
	return key.hasMeta (metaName) ? key.getMeta<std::string> (metaName) : defaultPrefix + indexStr;
}

static inline std::string getArgDescription (const kdb::Key & key, kdb_long_long_t index, const std::string & kind)
{
	auto indexStr = std::to_string (index);
	auto metaName = "gen/arg/description/#" + std::string (indexStr.length () - 1, '_') + indexStr;
	return key.hasMeta (metaName) ? key.getMeta<std::string> (metaName) :
					"Replaces occurence no. " + indexStr + " of " + kind + " in the keyname.";
}

static void getKeyArgsAndContext (const kdb::Key & key, const size_t parentKeyParts, kainjow::mustache::list & args,
				  std::vector<kainjow::mustache::object> & context, kainjow::mustache::list & contextAndArgs,
				  std::string & fmtString)
{
	using namespace kainjow::mustache;
	auto parts = getKeyParts (key);
	parts.erase (parts.begin (), parts.begin () + parentKeyParts);

	std::stringstream fmt;

	size_t pos = 1;
	size_t names = 1;
	size_t indices = 1;
	for (const auto & part : parts)
	{
		if (part == "_")
		{
			const std::string & argName = getArgName (key, names, "name");
			auto arg = object{ { "native_type", "const char *" },
					   { "name", argName },
					   { "index?", false },
					   { "description", getArgDescription (key, names, "_") } };
			args.push_back (arg);
			fmt << "%s/";
			++pos;
			++names;
			contextAndArgs.push_back (object{ { "code", argName } });
		}
		else if (part == "#")
		{
			const std::string & argName = getArgName (key, indices, "index");
			auto arg = object{ { "native_type", "kdb_long_long_t" },
					   { "name", argName },
					   { "index?", true },
					   { "description", getArgDescription (key, indices, "#") } };
			args.push_back (arg);
			fmt << "%*.*s%lld/";
			pos += 4;
			++indices;

			std::string argCode = "elektra_len (" + argName + "), elektra_len (";
			argCode += argName + "), \"#___________________\", (long long) ";
			argCode += argName;

			contextAndArgs.push_back (object{ { "code", argCode } });
		}
		else if (part.front () == '%' && part.back () == '%')
		{
			// contextual value
			auto name = part.substr (1, part.length () - 2);
			auto cName = name;
			escapeNonAlphaNum (cName);
			auto ctx = object{ { "name", name },
					   { "c_name", snakeCaseToCamelCase (cName) },
					   { "key_name", "system/elektra/codegen/context/" + name },
					   { "macro_name", snakeCaseToMacroCase (cName) },
					   { "tag_name", snakeCaseToPascalCase (cName) } };
			context.push_back (ctx);

			fmt << "%s/";
			++pos;

			contextAndArgs.push_back (object{ { "code", snakeCaseToCamelCase (cName) } });
		}
		else
		{
			// escape backslashes first too avoid collision
			fmt << std::regex_replace (part, std::regex ("[\\\\/]"), "\\\\$0") << "/";
		}
	}

	if (!args.empty ())
	{
		args.back ()["last?"] = true;
	}
	else if (!context.empty ())
	{
		// keep comma if args present as well
		context.back ()["last?"] = true;
	}

	if (!contextAndArgs.empty ())
	{
		contextAndArgs.back ()["last?"] = true;
	}

	if (args.size () > 1)
	{
		args[args.size () - 2]["last_but_one?"] = true;
	}

	if (contextAndArgs.size () > 1)
	{
		contextAndArgs[contextAndArgs.size () - 2]["last_but_one?"] = true;
	}

	fmtString = fmt.str ();
	fmtString.pop_back ();
}

kainjow::mustache::data ElektraGenTemplate::getTemplateData (const std::string & outputName, const kdb::KeySet & ks,
							     const std::string & parentKey) const
{
	if (parentKey.substr (0, 5) != "spec/")
	{
		throw CommandAbortException ("parentKey has to be in spec namespace");
	}

	using namespace kainjow::mustache;


	auto headerFile = outputName + ".h";
	auto includeGuard = createIncludeGuard (headerFile);
	auto initFunctionName = getParameter (Params::InitFunctionName, "loadConfiguration");
	auto helpFunctionName = getParameter (Params::HelpFunctionName, "printHelpMessage");
	auto specloadFunctionName = getParameter (Params::SpecloadFunctionName, "specloadCheck");
	auto additionalHeaders = split (getParameter (Params::AdditionalHeaders), ',');
	auto enumConversionString = getParameter (Params::EnumConversion, "default");

	auto enumConversion = EnumConversion::Default;
	if (enumConversionString == "trie")
	{
		enumConversion = EnumConversion::Trie;
	}
	else if (enumConversionString == "strcmp")
	{
		enumConversion = EnumConversion::Strcmp;
	}
	else if (enumConversionString != "default")
	{
		throw CommandAbortException ("enumConv must be one of: default, trie, strcmp");
	}


	auto cascadingParent = parentKey.substr (4);

	auto data = object{ { "header_file", headerFile },
			    { "include_guard", includeGuard },
			    { "spec_parent_key", parentKey },
			    { "parent_key", cascadingParent },
			    { "init_function_name", initFunctionName },
			    { "help_function_name", helpFunctionName },
			    { "specload_function_name", specloadFunctionName },
			    { "more_headers", list (additionalHeaders.begin (), additionalHeaders.end ()) } };

	list enums;
	list structs;
	list keys;

	auto specParent = kdb::Key (parentKey, KEY_END);

	EnumProcessor enumProcessor (enumConversion);
	StructProcessor structProcessor (specParent, ks);

	auto parentLength = specParent.getName ().length ();

	kdb::KeySet spec;

	kdb::Key parent = ks.lookup (specParent).dup ();
	parent.setName ("");
	spec.append (parent);

	auto parentKeyParts = getKeyParts (specParent);

	std::map<std::string, object> contexts;
	for (auto it = ks.begin (); it != ks.end (); ++it)
	{
		kdb::Key key = *it;

		if (!key.isSpec () || !key.isBelow (specParent) || !hasType (key))
		{
			continue;
		}

		auto type = getType (key);
		auto name = key.getName ();
		name.erase (0, sizeof ("spec") - 1);

		std::string fmtString;
		list args;
		std::vector<object> context;
		list contextAndArgs;
		getKeyArgsAndContext (key, parentKeyParts.size (), args, context, contextAndArgs, fmtString);

		if (!key.hasMeta ("default"))
		{
			throw CommandAbortException ("The key '" + name + "' doesn't have a default value!");
		}

		kdb::Key specKey = key.dup ();
		specKey.setName (specKey.getName ().substr (parentLength));
		specKey.setString (key.getMeta<std::string> ("default"));
		spec.append (specKey);

		std::unordered_set<std::string> allowedTypes = { "enum",
								 "string",
								 "boolean",
								 "char",
								 "octet",
								 "short",
								 "unsigned_short",
								 "long",
								 "unsigned_long",
								 "long_long",
								 "unsigned_long_long",
								 "float",
								 "double",
								 "long_double",
								 "struct",
								 "struct_ref" };

		if (allowedTypes.find (type) == allowedTypes.end ())
		{
			auto msg = "The key '" + name;
			msg += "' has an unsupported type ('" + type + "')!";
			throw CommandAbortException (msg);
		}

		auto nativeType = type == "string" ? "const char *" : "kdb_" + type + "_t";
		auto typeName = snakeCaseToPascalCase (type);

		auto tagName = getTagName (key, specParent.getName ());

		auto isArray = key.getBaseName () == "#";

		object keyObject = { { "name", name.substr (cascadingParent.size () + 1) }, // + 2 to remove slash
				     { "native_type", nativeType },
				     { "macro_name", snakeCaseToMacroCase (tagName) },
				     { "tag_name", snakeCaseToPascalCase (tagName) },
				     { "type_name", typeName },
				     { "is_array?", isArray } };

		if (!args.empty ())
		{
			keyObject["args_or_context?"] = object{ { "context_and_args", contextAndArgs } };
			keyObject["args?"] = true;
			keyObject["args"] = args;
			keyObject["fmt_string"] = fmtString;
		}

		if (!context.empty ())
		{
			keyObject["args_or_context?"] = object{ { "context_and_args", contextAndArgs } };
			keyObject["context?"] = true;
			keyObject["context"] = list (context.begin (), context.end ());
			keyObject["fmt_string"] = fmtString;

			for (const auto & ctx : context)
			{
				auto ctxName = ctx.find ("name")->second.string_value ();
				contexts[ctxName] = object (ctx);
			}
		}

		if (isArray)
		{
			if (args.size () > 1)
			{
				// remove last argument and last part of format string
				auto arrayArgs = list{ args.begin (), args.end () - 1 };
				arrayArgs.back ()["last?"] = true;
				auto arrayContextAndArgs = list{ contextAndArgs.begin (), contextAndArgs.end () - 1 };
				arrayContextAndArgs.back ()["last?"] = true;

				keyObject["array_args_or_context?"] =
					object ({ { "args", arrayArgs },
						  { "context_and_args", arrayContextAndArgs },
						  { "context", list (context.begin (), context.end ()) },
						  { "fmt_string", fmtString.substr (0, fmtString.rfind ('/')) } });
			}
			// remove last part ('/#') from name
			keyObject["array_name"] = name.substr (cascadingParent.size () + 1, name.size () - cascadingParent.size () - 3);
		}

		if (type == "enum")
		{
			auto enumData = enumProcessor.process (key, tagName);

			keyObject["type_name"] = enumData["type_name"].string_value ();
			keyObject["native_type"] = enumData["native_type"].string_value ();

			if (enumData["new"].is_true ())
			{
				enums.emplace_back (enumData);
			}
		}
		else if (type == "struct_ref")
		{
			bool allocate;
			processStructRef (key, specParent, ks, tagName, typeName, nativeType, allocate);

			keyObject["type_name"] = typeName;
			keyObject["native_type"] = nativeType;
			keyObject["is_struct_ref?"] = true;
			keyObject["alloc?"] = allocate;
		}
		else if (type == "struct")
		{
			auto maxDepth = key.hasMeta ("gen/struct/depth") ? key.getMeta<kdb::short_t> ("gen/struct/depth") : 1;
			auto baseDepth = getKeyParts (key).size ();

			kdb::KeySet subkeys;
			for (auto cur = it + 1; cur != ks.end (); ++cur)
			{
				if (cur->isBelow (key))
				{
					auto parts = getKeyParts (*cur);
					if (parts.size () <= baseDepth + maxDepth)
					{
						if (std::any_of (parts.begin () + baseDepth, parts.end () - 1,
								 [](const std::string & s) { return s == "_" || s == "#"; }) ||
						    parts.back () == "_")
						{
							throw CommandAbortException ("struct cannot contain globbed keys (_, #).");
						}

						subkeys.append (*cur);
					}
					else if (parts.size () <= baseDepth + maxDepth + 1 && parts.back () == "#")
					{
						subkeys.append (*cur);
					}
				}
				else
				{
					break;
				}
			}

			auto structData = structProcessor.process (key, subkeys, tagName);

			keyObject["type_name"] = structData["type_name"].string_value ();
			keyObject["native_type"] = structData["native_type"].string_value ();
			keyObject["is_struct?"] = true;
			keyObject["alloc?"] = structData["alloc?"].is_true ();

			if (structData["new"].is_true ())
			{
				structs.emplace_back (structData);
			}
		}

		keys.emplace_back (keyObject);
	}

	// TODO: make configurable?
	auto specloadArg = "--elektra-spec";

	kdb::KeySet contract;
	contract.append (kdb::Key ("system/plugins/global/gopts", KEY_VALUE, "mounted", KEY_END));

	list totalContext;
	kdb::KeySet defaultContext;
	for (auto & p : contexts)
	{
		totalContext.push_back (p.second);
		const auto & keyName = p.second["key_name"].string_value ();
		// TODO: contextual defaults
		defaultContext.append (kdb::Key (keyName, KEY_VALUE, "", KEY_END));
	}

	data["keys_count"] = std::to_string (keys.size ());
	data["keys"] = keys;
	data["enums"] = enums;
	data["structs"] = structs;
	data["defaults"] = keySetToCCode (spec);
	data["contract"] = keySetToCCode (contract);
	data["specload_arg"] = specloadArg;
	data["total_context"] = totalContext;
	data["default_context"] = keySetToCCode (defaultContext);

	return data;
}

EnumTrie::EnumTrie (const std::set<std::pair<std::string, std::string>> & values)
{
	for (auto v = values.begin (); v != values.end ();)
	{
		char c = v->first.at (0);
		std::string p;
		if (c != '\0')
		{
			p += c;
		}
		std::set<std::pair<std::string, std::string>> vals;

		while (v != values.end () && v->first.at (0) == c)
		{
			vals.insert (*v);
			++v;
		}
		insert (p, vals);
	}
}

void EnumTrie::insert (const std::string & prefix, const std::set<std::pair<std::string, std::string>> & values)
{
	std::unique_ptr<EnumTrie> child (new EnumTrie ());
	if (values.size () == 1)
	{
		child->stringValue = values.begin ()->first;
		child->name = values.begin ()->second;
	}
	else
	{
		for (auto v = values.begin (); v != values.end ();)
		{
			if (v->first.size () == prefix.size ())
			{
				child->stringValue = values.begin ()->first;
				child->name = values.begin ()->second;
				++v;
				continue;
			}

			char c = v->first.at (prefix.size ());
			std::string p = prefix;
			p += c;
			std::set<std::pair<std::string, std::string>> vals;

			while (v != values.end () && (v->first.size () > prefix.size () ? v->first.at (prefix.size ()) : '\0') == c)
			{
				vals.insert (*v);
				++v;
			}
			child->insert (p, vals);
		}
	}

	children[prefix.back ()] = std::move (child);
}

std::string EnumTrie::createSwitch ()
{
	std::stringstream ss;
	createSwitch (ss, 0);
	return ss.str ();
}

bool EnumTrie::createSwitch (std::stringstream & ss, size_t index)
{
	if (children.empty ())
	{
		if (stringValue.empty ())
		{
			return false;
		}

		ss << "*variable = " << name << ";" << std::endl;
		ss << "return 1;" << std::endl;
		return false;
	}

	ss << "switch (string[" << index << "])" << std::endl;
	ss << "{" << std::endl;
	for (auto & child : children)
	{
		ss << "case '" << child.first << "':" << std::endl;
		if (child.second->createSwitch (ss, index + 1))
		{
			ss << "break;" << std::endl;
		}
	}
	ss << "}" << std::endl;

	if (!stringValue.empty ())
	{
		ss << "*variable = " << name << ";" << std::endl;
		ss << "return 1;" << std::endl;
		return false;
	}
	return true;
}

size_t EnumTrie::getDepth ()
{
	if (children.empty ())
	{
		return 1;
	}

	std::vector<size_t> childDepths;
	std::transform (children.begin (), children.end (), std::back_inserter (childDepths),
			[](const std::pair<const char, std::unique_ptr<EnumTrie>> & p) { return p.second->getDepth (); });

	return *std::max_element (childDepths.begin (), childDepths.end ()) + 1;
}
