#include "logonomic_cli_addon/launcher_approval.h"

#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/launcher_utilities.h"
#include "logonomic_cli_addon/sha256.h"
#include "launcher_utilities_p.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace logonomic::cli_addon {
namespace {

struct Token
{
    std::string value;
    std::size_t begin = 0;
    std::size_t end = 0;
};

struct Argument
{
    std::string                name;
    std::optional<std::string> value;
    bool                       attached = false;

    friend bool operator==(const Argument&, const Argument&) = default;
};

struct Variadic_span
{
    const Launcher_approval_variadic_argument* rule = nullptr;
    std::size_t option = 0;
    std::size_t last = 0;
    bool        attached = false;
};

struct Invocation
{
    std::vector<Token> tokens;
    std::vector<bool>  approval;
    std::vector<bool>  bypass;
    std::size_t        program = 0;
    std::size_t        insertion = 0;

    // Empty names denote positionals; absent and empty option values differ.
    std::vector<Argument>      arguments;
    std::vector<Variadic_span> variadics;
};

bool contains(const std::vector<std::string>& values, std::string_view value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

bool space(char character)
{
    return character == ' ' || character == '\t';
}

bool option_word(std::string_view value)
{
    return value.size() > 1 && value.front() == '-';
}

bool valid_variadic_value(std::string_view value, const Launcher_approval_variadic_argument& rule)
{
    if (rule.empty_values_allowed) {
        return true;
    }
    if (value.empty()) {
        return false;
    }
    return rule.value_separator == '\0' ||
        (value.front() != rule.value_separator && value.back() != rule.value_separator &&
            value.find(std::string(2, rule.value_separator)) == std::string_view::npos);
}

std::optional<std::string_view> bash_assignment_name(std::string_view script, std::size_t position)
{
    const auto letter = [](char value) {
        return value == '_' || (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
    };
    if (position == script.size() || !letter(script[position])) {
        return std::nullopt;
    }
    const auto begin = position++;
    while (position < script.size() &&
        (letter(script[position]) || (script[position] >= '0' && script[position] <= '9')))
    {
        ++position;
    }
    return position < script.size() && script[position] == '='
        ? std::optional(script.substr(begin, position - begin))
        : std::nullopt;
}

bool read_token(std::string_view script, std::string_view runner, std::size_t& position, Token& token,
    bool frozen_assignment = false, bool allow_empty = false)
{
    const bool direct     = runner == k_runner_direct;
    const bool bash       = runner == k_runner_bash;
    const bool powershell = runner == k_runner_powershell;
    char quote = '\0';
    token.begin = position;
    const bool powershell_quoted_word = powershell && (script[position] == '\'' || script[position] == '"');
    while (position < script.size()) {
        const char character = script[position];
        if (quote == '\0' && space(character)) {
            break;
        }
        if (direct && character == '"') {
            const std::size_t first = position;
            while (position < script.size() && script[position] == '"') {
                ++position;
            }
            const std::size_t count = position - first;
            token.value.append(count / 3, '"');
            if (count % 3 == 1) {
                quote = quote == '\0' ? '"' : '\0';
            }
            continue;
        }
        if ((runner == k_runner_cmd || powershell) && quote == '"' && character == '"' &&
            position + 1 < script.size() && script[position + 1] == '"')
        {
            // Native argument passing can reinterpret doubled quote characters.
            // Refuse this form so quoted prompts or settings cannot become options.
            return false;
        }
        if (powershell && quote == '\'' && character == quote &&
            position + 1 < script.size() && script[position + 1] == quote)
        {
            token.value += quote;
            position += 2;
            continue;
        }
        if (!direct && character == quote) {
            quote = '\0';
            ++position;
            // PowerShell starts a separate argument after a quote-leading
            // word; attaching an option prefix would instead join its suffix.
            if (powershell_quoted_word && position < script.size() && !space(script[position])) {
                return false;
            }
            continue;
        }
        if (!direct && quote == '\0' &&
            (character == '"' || (character == '\'' && (bash || powershell))))
        {
            quote = character;
            ++position;
            continue;
        }
        if (bash && character == '\\' && quote != '\'') {
            if (++position == script.size()) {
                return false;
            }
            const char escaped = script[position];
            if (quote == '"' && std::string_view("$`\"\\").find(escaped) == std::string_view::npos) {
                token.value += '\\';
            }
            token.value += escaped;
            ++position;
            continue;
        }
        if (!direct && quote != '\'' && (bash || powershell) &&
            (character == '$' || character == '`'))
        {
            return false;
        }
        if (runner == k_runner_cmd && (character == '%' || character == '!' || character == '^' ||
            (character == '\\' && position + 1 < script.size() && script[position + 1] == '"')))
        {
            return false;
        }
        if (!direct && quote == '\0') {
            const std::string_view operators = bash ? ";|&<>(){}[]*?!~#" :
                powershell ? ";|&<>(){}[],@#*?" : "&|<>()";
            if (operators.find(character) != std::string_view::npos &&
                !(frozen_assignment && character == '~'))
            {
                return false;
            }
        }
        token.value += character;
        ++position;
    }
    token.end = position;
    // Windows PowerShell's native argument passing reparses embedded double
    // quotes, which can split one literal operand into an invocation-level flag.
    if (powershell && token.value.find('"') != std::string::npos) {
        return false;
    }
    // The editor refuses empty words; literal bash projection can retain them.
    // PowerShell/cmd empty-argument conventions stay outside this grammar.
    return quote == '\0' && (allow_empty || !token.value.empty());
}

bool powershell_quoted_attachment(std::string_view script, const Token& token)
{
    if (script[token.begin] == '\'' || script[token.begin] == '"') {
        return true;
    }
    std::size_t position = token.begin + 2;
    if (script[position] == '=') {
        ++position;
    }
    if (position == token.end || (script[position] != '\'' && script[position] != '"')) {
        return false;
    }
    Token operand;
    return read_token(script, k_runner_powershell, position, operand) && position == token.end;
}

std::size_t trailing_backslashes(std::string_view value)
{
    std::size_t count = 0;
    while (count < value.size() && value[value.size() - count - 1] == '\\') {
        ++count;
    }
    return count;
}

bool project_powershell_native_word(std::string& value)
{
    // Quoting does not make PowerShell's complete stop-parsing word literal.
    if (value == "--%") {
        return false;
    }
    if (value.find_first_of(" \t") != std::string::npos) {
        // powershell.exe quotes whitespace-bearing native words without
        // escaping terminal backslashes. Odd runs consume the closing quote;
        // the CRT halves even runs. Other native whitespace is not admitted.
        const auto count = trailing_backslashes(value);
        if (count % 2 != 0) {
            return false;
        }
        value.resize(value.size() - count / 2);
    }
    return true;
}

std::string powershell_literal(std::string_view value)
{
    std::string result = "'";
    for (const char character : value) {
        if (character == '\'') {
            result += '\'';
        }
        result += character;
    }
    if (value.find_first_of(" \t") != std::string_view::npos) {
        // Invert the native-word projection before adding the shell quotes.
        result.append(trailing_backslashes(value), '\\');
    }
    return result + '\'';
}

std::optional<std::vector<Token>> tokenize(const Launcher_request& launcher, bool project_invocation = false)
{
    const std::string& script = launcher.script_body;
    if (launcher.runner_key == k_runner_direct || launcher.runner_key == k_runner_powershell) {
        // Qt's literal splitter and PowerShell recognize Unicode whitespace.
        // Keep this editor's ASCII span grammar bounded rather than absorbing
        // a native argument boundary into the preceding option value.
        constexpr std::string_view unicode_spaces[]{
            "\u0085", "\u00a0", "\u1680", "\u2000", "\u2001", "\u2002", "\u2003", "\u2004",
            "\u2005", "\u2006", "\u2007", "\u2008", "\u2009", "\u200a", "\u2028", "\u2029",
            "\u202f", "\u205f", "\u3000",
        };
        for (const auto separator : unicode_spaces) {
            if (script.find(separator) != std::string::npos) {
                return std::nullopt;
            }
        }
    }
    if (launcher.runner_key == k_runner_powershell) {
        // PowerShell treats typographic quotes as syntax. The bounded ASCII
        // parser must not mistake their quoted contents for separate options.
        constexpr std::string_view smart_quotes[]{
            "\u2018", "\u2019", "\u201a", "\u201b", "\u201c", "\u201d", "\u201e",
        };
        for (const auto quote : smart_quotes) {
            if (script.find(quote) != std::string::npos) {
                return std::nullopt;
            }
        }
    }
    for (const unsigned char character : script) {
        if ((character < 0x20 && character != '\t') || character == 0x7f) {
            return std::nullopt;
        }
    }
    std::vector<Token> tokens;
    std::size_t position = 0;
    bool assignment_prefix = true;
    while (position < script.size()) {
        if (space(script[position])) {
            ++position;
            continue;
        }
        if (launcher.runner_key == k_runner_powershell && tokens.empty() && script[position] == '&') {
            if (position + 1 == script.size() || !space(script[position + 1])) {
                return std::nullopt;
            }
            tokens.push_back({"&", position, position + 1});
            ++position;
            continue;
        }
        Token token;
        const auto assignment = project_invocation && assignment_prefix && launcher.runner_key == k_runner_bash
            ? bash_assignment_name(script, position) : std::nullopt;
        if (!read_token(script, launcher.runner_key, position, token,
                assignment.has_value(), project_invocation && launcher.runner_key == k_runner_bash) ||
            (launcher.runner_key == k_runner_powershell && !project_powershell_native_word(token.value)))
        {
            return std::nullopt;
        }
        assignment_prefix = assignment.has_value();
        tokens.push_back(std::move(token));
    }
    return tokens;
}

bool host_invocation_matches(const Launcher_request& launcher, std::string_view platform,
    const Launcher_invocation& invocation, std::span<const std::string> host_argv)
{
    if (host_argv.empty()) {
        return false;
    }
    if (launcher.runner_key == k_runner_direct) {
        return std::equal(invocation.argv.begin(), invocation.argv.end(), host_argv.begin(), host_argv.end());
    }
    const bool windows = platform.starts_with("windows-");
    const auto separator = host_argv.front().find_last_of(windows ? "/\\" : "/");
    auto program = host_argv.front().substr(separator == std::string::npos ? 0 : separator + 1);
    if (windows) {
        program = ascii_lowercase(program);
    }
    if (launcher.runner_key == k_runner_bash) {
        return (program == "bash" || (windows && program == "bash.exe")) && host_argv.size() == 3 &&
            host_argv[1] == (platform.starts_with("linux-") ? "-c" : "-lc") &&
            host_argv[2] == launcher.script_body;
    }
    if (launcher.runner_key == k_runner_powershell) {
        if (program != "powershell.exe") {
            return false;
        }
        const std::size_t options = host_argv.size() > 1 && host_argv[1] == "-NoExit" ? 2 : 1;
        return host_argv.size() == options + 3 && host_argv[options] == "-NoLogo" &&
            host_argv[options + 1] == "-Command" && host_argv[options + 2] == launcher.script_body;
    }
    return launcher.runner_key == k_runner_cmd && program == "cmd.exe" && host_argv.size() == 3 &&
        (host_argv[1] == "/c" || host_argv[1] == "/k") && host_argv[2] == launcher.script_body;
}

bool program_matches(const Token& program, const Launcher_approval_rules& rules, bool windows)
{
    const auto separator = program.value.find_last_of(windows ? "/\\" : "/");
    const std::string basename = program.value.substr(separator == std::string::npos ? 0 : separator + 1);
    for (const std::string& name : rules.program_names) {
        if (windows ? ascii_lowercase(basename) == ascii_lowercase(name) : basename == name) {
            return true;
        }
    }
    return false;
}

std::optional<Invocation> parse_invocation(const Launcher_request& launcher,
    const std::string& platform, const Launcher_approval_rules& rules)
{
    if (!is_valid_platform_id(platform) || !is_runner_supported_on_platform(launcher.runner_key, platform) ||
        !decode_launcher_request(encode_launcher_request(launcher), nullptr))
    {
        return std::nullopt;
    }
    auto tokens = tokenize(launcher);
    if (!tokens || tokens->empty()) {
        return std::nullopt;
    }
    const bool call_operator = launcher.runner_key == k_runner_powershell && tokens->front().value == "&";
    const std::size_t program_index = call_operator ? 1 : 0;
    if (program_index == tokens->size() ||
        !program_matches((*tokens)[program_index], rules, platform.starts_with("windows-")))
    {
        return std::nullopt;
    }
    const Token& program = (*tokens)[program_index];
    if (launcher.runner_key == k_runner_powershell && !call_operator &&
        launcher.script_body.substr(program.begin, program.end - program.begin).find_first_of("\"'") !=
            std::string::npos)
    {
        return std::nullopt;
    }

    Invocation invocation;
    invocation.tokens    = std::move(*tokens);
    invocation.approval  = std::vector<bool>(invocation.tokens.size(), false);
    invocation.bypass    = invocation.approval;
    invocation.program   = program_index;
    invocation.insertion = program_index;
    bool first_positional = true;
    for (std::size_t i = program_index + 1; i < invocation.tokens.size(); ++i) {
        const std::string& value = invocation.tokens[i].value;
        if (value == "--") {
            for (; i < invocation.tokens.size(); ++i) {
                invocation.arguments.push_back({"", invocation.tokens[i].value});
            }
            break;
        }
        if (value.front() != '-' || value == "-") {
            invocation.arguments.push_back({"", value});
            if (first_positional) {
                if (contains(rules.unsupported_commands, value)) {
                    return std::nullopt;
                }
                if (contains(rules.session_commands, value)) {
                    invocation.insertion = i;
                }
                first_positional = false;
            }
            continue;
        }
        const auto equals = value.find('=');
        if ((!rules.inactive_bypass_argument.empty() && value == rules.inactive_bypass_argument) ||
            (equals != std::string::npos && value == rules.canonical_argument))
        {
            invocation.approval[i] = true;
            invocation.bypass[i]   = value == rules.canonical_argument;
            continue;
        }
        std::string_view name = std::string_view(value).substr(0, equals);
        bool attached         = equals != std::string::npos;
        std::size_t value_offset = attached ? equals + 1 : value.size();
        auto variadic = std::find_if(rules.variadic_arguments.begin(), rules.variadic_arguments.end(),
            [name](const auto& entry) { return name == entry.argument; });
        if (variadic == rules.variadic_arguments.end()) {
            variadic = std::find_if(rules.variadic_arguments.begin(), rules.variadic_arguments.end(),
                [&value](const auto& entry) {
                    return entry.argument.size() == 2 && value.size() > 2 &&
                        value.starts_with(entry.argument);
                });
            if (variadic != rules.variadic_arguments.end()) {
                name = variadic->argument;
                attached = true;
                value_offset = 2;
            }
        }
        if (variadic != rules.variadic_arguments.end()) {
            // PowerShell's unquoted parameter-token grammar can split a short
            // attachment at punctuation that is literal in a quoted word.
            if (attached && name.size() == 2 && launcher.runner_key == k_runner_powershell &&
                !powershell_quoted_attachment(launcher.script_body, invocation.tokens[i]))
            {
                return std::nullopt;
            }
            const std::size_t option = i;
            if (attached) {
                const auto operand = std::string_view(value).substr(value_offset);
                if (!valid_variadic_value(operand, *variadic)) {
                    return std::nullopt;
                }
                invocation.arguments.push_back({std::string(name), std::string(operand)});
            }
            if (!attached || !variadic->attached_value_terminates) {
                while (i + 1 < invocation.tokens.size() && !option_word(invocation.tokens[i + 1].value)) {
                    const auto& operand = invocation.tokens[++i].value;
                    if (!valid_variadic_value(operand, *variadic)) {
                        return std::nullopt;
                    }
                    invocation.arguments.push_back({std::string(name), operand});
                }
            }
            if (!attached && i == option) {
                // Some native parsers swallow the first option-looking value,
                // while their raw argv consumers still act on it as an option.
                if (variadic->requires_value) {
                    return std::nullopt;
                }
                invocation.arguments.push_back({std::string(name), std::nullopt});
            }
            invocation.variadics.push_back({&*variadic, option, i, attached});
            continue;
        }
        const auto mode = std::find_if(rules.approval_modes.begin(), rules.approval_modes.end(),
            [name](const Launcher_approval_mode& entry) { return name == entry.argument; });
        const bool bypass = name == rules.canonical_argument || contains(rules.bypass_arguments, name);
        const bool conflict = contains(rules.conflicting_arguments, name);
        if (bypass || conflict || contains(rules.flag_arguments, name)) {
            if (attached) {
                return std::nullopt;
            }
            if (rules.flag_arguments_consume_booleans && i + 1 < invocation.tokens.size()) {
                const std::string& next = invocation.tokens[i + 1].value;
                if (next == "true" || next == "false") {
                    return std::nullopt;
                }
            }
            invocation.approval[i] = bypass || conflict;
            invocation.bypass[i]   = bypass;
            if (!invocation.approval[i]) {
                invocation.arguments.push_back({std::string(name), std::nullopt});
            }
            continue;
        }
        const bool required_value = mode != rules.approval_modes.end() || contains(rules.value_arguments, name);
        const bool optional_value = contains(rules.optional_value_arguments, name);
        if (!required_value && !optional_value) {
            return std::nullopt;
        }
        const std::size_t argument_index = i;
        std::optional<std::string> argument_value;
        if (attached) {
            argument_value = value.substr(equals + 1);
        }
        else
        if (i + 1 < invocation.tokens.size() &&
            invocation.tokens[i + 1].value != "--" &&
            (required_value || invocation.tokens[i + 1].value.front() != '-'))
        {
            if (required_value && !rules.value_arguments_consume_options &&
                invocation.tokens[i + 1].value.front() == '-')
            {
                return std::nullopt;
            }
            argument_value = invocation.tokens[++i].value;
        }
        else
        if (required_value) {
            return std::nullopt;
        }
        if (mode != rules.approval_modes.end()) {
            const bool bypass_mode = contains(mode->bypass_values, *argument_value);
            if (!bypass_mode && !contains(mode->other_values, *argument_value)) {
                return std::nullopt;
            }
            for (std::size_t index = argument_index; index <= i; ++index) {
                invocation.approval[index] = true;
                invocation.bypass[index]   = bypass_mode;
            }
        }
        else {
            invocation.arguments.push_back({std::string(name), std::move(argument_value), attached});
        }
    }
    return invocation;
}

struct Edit
{
    std::size_t begin = 0;
    std::size_t end = 0;
    std::string replacement;
};

std::string edit_script(const std::string& script, const Invocation& invocation,
    const std::vector<bool>& removed, const std::string& inserted,
    const std::vector<std::string>& boundaries, std::vector<Edit> edits)
{
    for (std::size_t i = 1; i < invocation.tokens.size(); ++i) {
        if (!removed[i]) {
            continue;
        }
        const std::size_t begin = invocation.tokens[i - 1].end;
        const std::string replacement = boundaries[i].empty() ? std::string{} : " " + boundaries[i];
        while (i + 1 < invocation.tokens.size() && removed[i + 1]) {
            ++i;
        }
        edits.push_back({begin, invocation.tokens[i].end, replacement});
    }
    if (!inserted.empty()) {
        const auto position = invocation.tokens[invocation.insertion].end;
        edits.push_back({position, position, " " + inserted});
    }
    std::sort(edits.begin(), edits.end(), [](const Edit& left, const Edit& right) {
        return left.begin > right.begin || (left.begin == right.begin && left.end > right.end);
    });
    std::string result = script;
    for (const Edit& edit : edits) {
        result.replace(edit.begin, edit.end - edit.begin, edit.replacement);
    }
    return result;
}

} // namespace

std::optional<Launcher_invocation> literal_launcher_invocation(
    const Launcher_request& launcher, std::string_view platform_id,
    std::optional<std::span<const std::string>> host_argv)
{
    if (!is_valid_platform_id(platform_id) || !is_runner_supported_on_platform(launcher.runner_key, platform_id) ||
        !decode_launcher_request(encode_launcher_request(launcher), nullptr))
    {
        return std::nullopt;
    }
    Launcher_invocation result;
    if (launcher.runner_key == k_runner_direct) {
        auto argv = launcher_detail::direct_argv(launcher.script_body);
        if (!argv) {
            return std::nullopt;
        }
        result.argv = std::move(*argv);
    }
    else {
        const auto tokens = tokenize(launcher, true);
        if (!tokens || tokens->empty()) {
            return std::nullopt;
        }
        std::size_t program = 0;
        if (launcher.runner_key == k_runner_bash) {
            while (program < tokens->size()) {
                const auto name = bash_assignment_name(launcher.script_body, (*tokens)[program].begin);
                if (!name) {
                    break;
                }
                result.environment_names.emplace_back(*name);
                ++program;
            }
        }
        if (launcher.runner_key == k_runner_powershell) {
            const bool call_operator = tokens->front().value == "&";
            program = call_operator ? 1 : 0;
            if (program == tokens->size()) {
                return std::nullopt;
            }
            const auto& token = (*tokens)[program];
            if (!call_operator &&
                launcher.script_body.substr(token.begin, token.end - token.begin).find_first_of("\"'") !=
                    std::string::npos)
            {
                return std::nullopt;
            }
            for (std::size_t index = program + 1; index < tokens->size(); ++index) {
                const auto& argument = (*tokens)[index];
                if (argument.value.size() > 2 && argument.value.front() == '-' && argument.value[1] != '-' &&
                    !powershell_quoted_attachment(launcher.script_body, argument))
                {
                    return std::nullopt;
                }
            }
        }
        for (; program < tokens->size(); ++program) {
            result.argv.push_back((*tokens)[program].value);
        }
    }
    if (result.argv.empty() || (host_argv && !host_invocation_matches(launcher, platform_id, result, *host_argv))) {
        return std::nullopt;
    }
    return result;
}

Launcher_approval_transform_result transform_launcher_approval(
    const Launcher_approval_transform_request& request, const Launcher_approval_rules& rules)
{
    const auto invocation = parse_invocation(request.launcher, request.platform_id, rules);
    if (!invocation) {
        return {};
    }
    const bool yolofy = request.action == Launcher_approval_action::YOLOFY;
    const auto& removed = yolofy ? invocation->approval : invocation->bypass;
    const auto removal_count = std::count(removed.begin(), removed.end(), true);
    std::size_t canonical_count = 0;
    for (std::size_t i = 0; i < invocation->tokens.size(); ++i) {
        if (invocation->bypass[i] && invocation->tokens[i].value == rules.canonical_argument) {
            ++canonical_count;
        }
    }
    if ((!yolofy && removal_count == 0) || (yolofy && removal_count == 1 && canonical_count == 1)) {
        return {Launcher_approval_disposition::UNCHANGED, request.launcher.script_body};
    }
    std::vector<Edit> edits;
    std::vector<std::string> boundaries(invocation->tokens.size());
    bool boundary_has_canonical = false;
    for (const auto& span : invocation->variadics) {
        if (span.attached && span.rule->attached_value_terminates) {
            continue;
        }
        const std::size_t boundary = span.last + 1;
        std::size_t next = boundary;
        while (next < removed.size() && removed[next]) {
            ++next;
        }
        if (next == boundary || next == removed.size() || option_word(invocation->tokens[next].value)) {
            continue;
        }
        if (span.rule->attached_value_terminates && span.last != span.option) {
            const auto& operand = invocation->tokens[span.last];
            const auto begin = span.last == span.option + 1
                ? invocation->tokens[span.option].begin
                : operand.begin;
            if (request.launcher.runner_key == k_runner_powershell && span.rule->argument.size() == 2) {
                edits.push_back({begin, operand.end,
                    powershell_literal(span.rule->argument + "=" + operand.value)});
            }
            else {
                edits.push_back({begin, operand.begin, span.rule->argument + "="});
            }
        }
        else
        if (yolofy && !boundary_has_canonical) {
            boundaries[boundary] = rules.canonical_argument;
            boundary_has_canonical = true;
        }
        else
        if (!yolofy && !rules.inactive_bypass_argument.empty()) {
            boundaries[boundary] = rules.inactive_bypass_argument;
        }
        else {
            return {};
        }
    }
    std::string script = edit_script(request.launcher.script_body, *invocation, removed,
        yolofy && !boundary_has_canonical ? rules.canonical_argument : std::string{}, boundaries, std::move(edits));
    if (script.size() > limits::k_max_script_body_bytes) {
        return {};
    }
    // Approval edits can expose a positional to a neighboring option. Valid
    // syntax alone does not preserve which option owns each surviving value.
    Launcher_request edited = request.launcher;
    edited.script_body = script;
    const auto edited_invocation = parse_invocation(edited, request.platform_id, rules);
    if (!edited_invocation || edited_invocation->arguments != invocation->arguments) {
        return {};
    }
    return {Launcher_approval_disposition::CHANGED, std::move(script)};
}

Launcher_approval_comparison_result launcher_approval_comparison_key(
    const Launcher_approval_comparison_request& request, const Launcher_approval_rules& rules)
{
    const auto invocation = parse_invocation(request.launcher, request.platform_id, rules);
    if (!invocation) {
        return {};
    }
    Json_value identity = Json_value::array();
    identity.as_array().push_back(Json_value(request.platform_id));
    identity.as_array().push_back(Json_value(request.launcher.runner_key));
    identity.as_array().push_back(Json_value(request.launcher.environment_text));
    for (std::size_t i = 0; i <= invocation->program; ++i) {
        identity.push_back(Json_value(invocation->tokens[i].value));
    }
    for (const auto& argument : invocation->arguments) {
        Json_value operand = Json_value::array();
        operand.push_back(Json_value(argument.name));
        operand.push_back(argument.value ? Json_value(*argument.value) : Json_value{});
        operand.push_back(Json_value(argument.attached));
        identity.push_back(std::move(operand));
    }
    return {true, to_hex(Sha256::digest(serialize_json_canonical(identity)))};
}

} // namespace logonomic::cli_addon
