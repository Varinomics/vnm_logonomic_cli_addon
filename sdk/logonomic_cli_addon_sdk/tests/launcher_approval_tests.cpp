#include "sdk_test_support.h"

#include "logonomic_cli_addon/launcher_approval.h"
#include "logonomic_cli_addon/launcher_utilities.h"
#include "logonomic_cli_addon/limits.h"

#include <string>
#include <utility>
#include <vector>

namespace sdk_tests {
namespace {

namespace addon = logonomic::cli_addon;
using Action = addon::Launcher_approval_action;
using Disposition = addon::Launcher_approval_disposition;

addon::Launcher_approval_rules rules()
{
    addon::Launcher_approval_rules result;
    result.program_names           = {"work-cli", "work-cli.exe", "work-cli.cmd"};
    result.canonical_argument      = "--approve-all";
    result.bypass_arguments        = {"--no-confirm", "-y"};
    result.approval_modes          = {{"--approval", {"bypass"}, {"normal", "plan"}},
                                     {"--sandbox", {}, {"read-only", "unrestricted"}}};
    result.conflicting_arguments   = {"--confirm"};
    result.value_arguments         = {"--model", "--profile", "--settings", "--prompt", "-c"};
    result.optional_value_arguments = {"--resume"};
    result.flag_arguments          = {"--verbose", "--allow-approve-all"};
    result.session_commands        = {"resume", "run"};
    result.unsupported_commands    = {"login", "logout", "config"};
    return result;
}

addon::Launcher_approval_transform_request request(std::string script, Action action = Action::YOLOFY,
    std::string runner = addon::k_runner_direct, std::string platform = "linux-x86_64")
{
    return {{std::move(script), std::move(runner), "WORK_PROFILE=personal\nWORK_RULES=strict"},
        std::move(platform), action};
}

addon::Launcher_approval_comparison_result key(const addon::Launcher_approval_transform_request& value,
    const addon::Launcher_approval_rules& policy = rules())
{
    return addon::launcher_approval_comparison_key({value.launcher, value.platform_id}, policy);
}

bool literal_launcher_projection()
{
    const addon::Launcher_request direct{R"(work-cli --rules """quoted""" '')", addon::k_runner_direct, {}};
    const std::vector<std::string> direct_argv{"work-cli", "--rules", "\"quoted\"", "''"};
    const auto direct_result = addon::literal_launcher_invocation(direct, "linux-x86_64", direct_argv);
    SDK_CHECK(direct_result && direct_result->argv == direct_argv && direct_result->environment_names.empty(),
        "direct projection uses the host grammar, preserving triple quotes and literal single quotes");
    auto mismatched = direct_argv;
    mismatched.back() = "changed";
    SDK_CHECK(!addon::literal_launcher_invocation(direct, "linux-x86_64", mismatched),
        "direct projection binds every host argument");
    SDK_CHECK(!addon::literal_launcher_invocation(direct, "linux-x86_64", std::span<const std::string>{}),
        "present empty host evidence differs from recognition without evidence");
    // vnm_framework cpp/core/vnm_launcher_command.cpp rejects unbalanced
    // quotes and runs of two quotes before calling QProcess::splitCommand.
    for (const auto* script : {"work-cli --rules \"unfinished", "work-cli --rules \"\"",
        "work-cli --rules part\"\"value", "\" \" --rules value"})
    {
        SDK_CHECK(!addon::literal_launcher_invocation({script, addon::k_runner_direct, {}}, "linux-x86_64"),
            "direct projection must apply the host's quote and blank-program validation");
    }
    const addon::Launcher_request unicode{
        "work-cli\u00a0--rules\u2003\"inside\u00a0value\"", addon::k_runner_direct, {}};
    const std::vector<std::string> unicode_argv{"work-cli", "--rules", "inside\u00a0value"};
    const auto unicode_result = addon::literal_launcher_invocation(unicode, "linux-x86_64", unicode_argv);
    SDK_CHECK(unicode_result && unicode_result->argv == unicode_argv,
        "direct Unicode separators split words while quoted Unicode whitespace remains data");

    const addon::Launcher_request bash{
        R"(PROFILE=~/native RULES='literal ~' work-cli --rules "one\\two" --model model\ name)", addon::k_runner_bash, {}};
    const std::vector<std::string> bash_host{"/bin/bash", "-c", bash.script_body};
    const auto bash_result = addon::literal_launcher_invocation(bash, "linux-x86_64", bash_host);
    SDK_CHECK(bash_result && bash_result->argv ==
        std::vector<std::string>({"work-cli", "--rules", R"(one\two)", "--model", "model name"}) &&
        bash_result->environment_names == std::vector<std::string>({"PROFILE", "RULES"}),
        "bash escape removal preserves native words and leaves assignment values to frozen host evidence");
    auto different_script = bash_host;
    different_script.back() += " --other";
    SDK_CHECK(!addon::literal_launcher_invocation(bash, "linux-x86_64", different_script),
        "shell projection binds the host's exact script");
    SDK_CHECK(addon::transform_launcher_approval({bash, "linux-x86_64", Action::YOLOFY}, rules()).disposition ==
        Disposition::UNSUPPORTED, "projection assignment support does not widen the approval editor's grammar");

    const addon::Launcher_request powershell{
        R"(& 'C:\Program Files\work-cli.exe' --rules 'Don''t guess' --cwd 'C:\work area\\')",
        addon::k_runner_powershell, {}};
    const std::vector<std::string> powershell_host{
        "powershell.exe", "-NoExit", "-NoLogo", "-Command", powershell.script_body};
    const auto powershell_result = addon::literal_launcher_invocation(powershell, "windows-x86_64", powershell_host);
    SDK_CHECK(powershell_result && powershell_result->argv == std::vector<std::string>({
        R"(C:\Program Files\work-cli.exe)", "--rules", "Don't guess", "--cwd", R"(C:\work area\)"}),
        "PowerShell doubled single quotes and even trailing backslashes project to native words");
    for (const auto* script : {
        "'work-cli' --rules literal", "work-cli --rules \"$env:RULES\"", "work-cli --% --rules literal",
        R"(work-cli --rules 'two words\')", R"(work-cli --rules 'embedded "quote"')"})
    {
        SDK_CHECK(!addon::literal_launcher_invocation({script, addon::k_runner_powershell, {}}, "windows-x86_64"),
            "PowerShell syntax outside the native literal subset remains unavailable");
    }
    for (const auto* script : {"work-cli --rules $(other)", "work-cli; other", "work-cli --rules ~", "work-cli *.txt"}) {
        SDK_CHECK(!addon::literal_launcher_invocation({script, addon::k_runner_bash, {}}, "linux-x86_64"),
            "bash expansions and compound commands are never evaluated during projection");
    }
    const addon::Launcher_request cmd{R"(work-cli --rules "literal value")", addon::k_runner_cmd, {}};
    const std::vector<std::string> cmd_host{"cmd.exe", "/c", cmd.script_body};
    const auto cmd_result = addon::literal_launcher_invocation(cmd, "windows-x86_64", cmd_host);
    SDK_CHECK(cmd_result && cmd_result->argv == std::vector<std::string>({"work-cli", "--rules", "literal value"}),
        "the supported literal cmd invocation retains its words");
    return true;
}

bool powershell_native_words()
{
    // Oracle: Windows PowerShell 5.1 quotes whitespace-bearing native words;
    // the CRT halves even terminal backslash runs and merges odd runs with
    // following arguments. The exact --% word also controls native parsing.
    auto policy = rules();
    policy.variadic_arguments = {{"--image", true, true}, {"-i", true, true}};
    for (const auto* script : {
            R"(work-cli -i 'C:\design shots\' --no-confirm describe)",
            R"(work-cli --image='C:\design shots\\\' --no-confirm describe)",
            R"(work-cli --model 'C:\model files\' --no-confirm describe)",
            R"(work-cli 'two words\' --no-confirm)",
            R"(work-cli --approve-all --model 'two words\')",
            R"(work-cli --model 'two words\' describe)",
            "work-cli --model --% --no-confirm 'two words'",
            "work-cli --model '--%' --no-confirm 'two words'",
            "work-cli --no-confirm -- \"--%\" 'two words'"})
    {
        for (const auto action : {Action::YOLOFY, Action::UNYOLOFY}) {
            const auto input = request(script, action, addon::k_runner_powershell, "windows-x86_64");
            SDK_CHECK(addon::transform_launcher_approval(input, policy).disposition == Disposition::UNSUPPORTED &&
                !key(input, policy).supported,
                "native control or merged words are rejected before option roles and unchanged results: " +
                    input.launcher.script_body);
        }
    }
    const std::vector<std::pair<std::string, std::string>> cases{
        {R"(work-cli -i 'C:\design shots\\' --no-confirm describe)",
            R"(work-cli '-i=C:\design shots\\' describe)"},
        {R"(work-cli -i first.png 'C:\owner''s shots\\\\' --no-confirm describe)",
            R"(work-cli -i first.png '-i=C:\owner''s shots\\\\' describe)"},
        {R"(work-cli --image 'C:\design shots\\' --no-confirm describe)",
            R"(work-cli --image='C:\design shots\\' describe)"},
        {"work-cli -i 'design\tshots\\\\' --no-confirm describe",
            "work-cli '-i=design\tshots\\\\' describe"},
        {R"(work-cli -i 'C:\shots\' --no-confirm describe)",
            R"(work-cli '-i=C:\shots\' describe)"},
        {R"(work-cli -i='C:\design shots\\' --no-confirm describe)",
            R"(work-cli -i='C:\design shots\\' describe)"},
        {R"(work-cli --image='C:\design shots\\\\' --no-confirm describe)",
            R"(work-cli --image='C:\design shots\\\\' describe)"},
        {"work-cli -i='--%' --no-confirm describe", "work-cli -i='--%' describe"},
        {"work-cli '-i=--%' --no-confirm describe", "work-cli '-i=--%' describe"},
        {"work-cli --model=--% --no-confirm describe", "work-cli --model=--% describe"},
        {"work-cli --model 'literal--%data' --no-confirm describe", "work-cli --model 'literal--%data' describe"},
    };
    for (const auto& [before, after] : cases) {
        auto input = request(before, Action::UNYOLOFY, addon::k_runner_powershell, "windows-x86_64");
        const auto result = addon::transform_launcher_approval(input, policy);
        const auto before_key = key(input, policy);
        SDK_CHECK(result.disposition == Disposition::CHANGED && result.script_body == after,
            "PowerShell native words survive attachment: " + before + " => " + result.script_body);
        input.launcher.script_body = result.script_body;
        SDK_CHECK(addon::transform_launcher_approval(input, policy).disposition == Disposition::UNCHANGED &&
            before_key.supported && key(input, policy).supported && before_key.key == key(input, policy).key,
            "native-word projection preserves comparison identity and idempotence");
    }
    const auto one = key(request(R"(work-cli -i 'C:\design shots\\' --no-confirm describe)",
        Action::UNYOLOFY, addon::k_runner_powershell, "windows-x86_64"), policy);
    const auto two = key(request(R"(work-cli -i 'C:\design shots\\\\' --no-confirm describe)",
        Action::UNYOLOFY, addon::k_runner_powershell, "windows-x86_64"), policy);
    SDK_CHECK(one.supported && two.supported && one.key != two.key,
        "different native trailing backslash values have distinct comparison keys");
    return true;
}

bool variadic_approval_edits()
{
    // Oracle: native clap 4.5.58 and Claude's Commander parser terminate an
    // attached variadic occurrence and append repeated occurrences in order.
    auto policy = rules();
    policy.variadic_arguments = {
        {"--image", true, true, false, ','},
        {"-i",      true, true, false, ','},
        {"--tools", true, true},
    };
    struct Case
    {
        std::string before;
        std::string after;
        Action      action = Action::UNYOLOFY;
        std::string runner = addon::k_runner_direct;
        std::string platform = "linux-x86_64";
    };
    const std::vector<Case> cases{
        {"work-cli --image shot.png --no-confirm describe", "work-cli --image=shot.png describe"},
        {"work-cli --image one.png \"second shot.png\" --approval bypass describe --model fast",
            "work-cli --image one.png --image=\"second shot.png\" describe --model fast"},
        {"work-cli --image one.png,two.png --approval plan describe",
            "work-cli --approve-all --image=one.png,two.png describe", Action::YOLOFY},
        {"work-cli --image shot.png --no-confirm resume session-51",
            "work-cli --image=shot.png resume --approve-all session-51", Action::YOLOFY},
        {"work-cli resume session-51 -i shot.png --no-confirm describe",
            "work-cli resume session-51 -i=shot.png describe"},
        {"work-cli -ishot.png --no-confirm describe", "work-cli -ishot.png describe"},
        {"work-cli -i=shot.png --no-confirm describe", "work-cli -i=shot.png describe"},
        {"work-cli --image one.png --image two.png --no-confirm describe",
            "work-cli --image one.png --image=two.png describe"},
        {"work-cli --image shot.png --no-confirm", "work-cli --image shot.png"},
        {"work-cli --image shot.png --no-confirm -- --literal", "work-cli --image shot.png -- --literal"},
        {"work-cli --image shot.png --no-confirm --model fast describe",
            "work-cli --image shot.png --model fast describe"},
        {"work-cli describe --image shot.png --no-confirm", "work-cli describe --image shot.png"},
        {"work-cli --tools= --no-confirm describe", "work-cli --tools= describe"},
        {"work-cli --tools=--bare --no-confirm describe", "work-cli --tools=--bare describe"},
        {"work-cli --image - --no-confirm describe", "work-cli --image=- describe"},
        {"work-cli \"--image\" \"design shot.png\" --no-confirm describe",
            "work-cli --image=\"design shot.png\" describe"},
        {"work-cli --image 'design shot.png' --no-confirm describe",
            "work-cli --image='design shot.png' describe", Action::UNYOLOFY, addon::k_runner_bash},
        {"work-cli \"--image\" \"design shot.png\" --no-confirm describe",
            "work-cli --image=\"design shot.png\" describe", Action::UNYOLOFY,
            addon::k_runner_cmd, "windows-x86_64"},
        {"work-cli \"--image\" \"design shot.png\" --no-confirm describe",
            "work-cli --image=\"design shot.png\" describe", Action::UNYOLOFY,
            addon::k_runner_powershell, "windows-x86_64"},
        {"work-cli --tools 'owner''s directory' --no-confirm describe",
            "work-cli --tools='owner''s directory' describe", Action::UNYOLOFY,
            addon::k_runner_powershell, "windows-x86_64"},
        {"work-cli -i shot.png --no-confirm describe",
            "work-cli '-i=shot.png' describe", Action::UNYOLOFY,
            addon::k_runner_powershell, "windows-x86_64"},
        {"work-cli -i first.png \"second shot.png\" --no-confirm describe",
            "work-cli -i first.png '-i=second shot.png' describe", Action::UNYOLOFY,
            addon::k_runner_powershell, "windows-x86_64"},
        {"work-cli -i 'owner''s shot.png' --no-confirm describe",
            "work-cli '-i=owner''s shot.png' describe", Action::UNYOLOFY,
            addon::k_runner_powershell, "windows-x86_64"},
        {"work-cli -i \"one.png,two.png\" --approval plan describe",
            "work-cli --approve-all '-i=one.png,two.png' describe", Action::YOLOFY,
            addon::k_runner_powershell, "windows-x86_64"},
        {"work-cli '-ishot.png' --no-confirm describe", "work-cli '-ishot.png' describe", Action::UNYOLOFY,
            addon::k_runner_powershell, "windows-x86_64"},
        {"work-cli \"-i=shot.png\" --no-confirm describe", "work-cli \"-i=shot.png\" describe", Action::UNYOLOFY,
            addon::k_runner_powershell, "windows-x86_64"},
        {"work-cli -i=\"shot.png\" --no-confirm describe", "work-cli -i=\"shot.png\" describe", Action::UNYOLOFY,
            addon::k_runner_powershell, "windows-x86_64"},
        {"work-cli -i'shot.png' --no-confirm describe", "work-cli -i'shot.png' describe", Action::UNYOLOFY,
            addon::k_runner_powershell, "windows-x86_64"},
        {"work-cli -i='--shot.png' --no-confirm describe", "work-cli -i='--shot.png' describe", Action::UNYOLOFY,
            addon::k_runner_powershell, "windows-x86_64"},
    };
    for (const auto& value : cases) {
        auto input = request(value.before, value.action, value.runner, value.platform);
        const auto result = addon::transform_launcher_approval(input, policy);
        SDK_CHECK(result.disposition == Disposition::CHANGED && result.script_body == value.after,
            "variadic operands keep their native owner: " + value.before + " => " + result.script_body);
        const auto before_key = key(input, policy);
        input.launcher.script_body = result.script_body;
        const auto repeated = addon::transform_launcher_approval(input, policy);
        SDK_CHECK(repeated.disposition == Disposition::UNCHANGED && repeated.script_body == value.after,
            "variadic approval edits are idempotent");
        SDK_CHECK(before_key.supported && key(input, policy).supported && before_key.key == key(input, policy).key,
            "safe attached spelling preserves comparison identity");
    }
    for (const auto* script : {
            "work-cli --image", "work-cli --image=", "work-cli --image one.png,,two.png",
            "work-cli --image --no-confirm describe", "work-cli --tools --bare --no-confirm describe",
            "work-cli --tools -- describe"})
    {
        const auto input = request(script, Action::UNYOLOFY);
        SDK_CHECK(addon::transform_launcher_approval(input, policy).disposition == Disposition::UNSUPPORTED &&
            !key(input, policy).supported,
            "missing values and raw option-looking first values are unsupported: " + input.launcher.script_body);
    }
    // Oracle: Windows PowerShell 5.1 native argv capture splits a quote-leading
    // word from its bare suffix, but an attached option prefix joins them.
    for (const auto* operand : {"'foo'bar", "\"foo\"bar", "'foo bar'baz"}) {
        for (const auto action : {Action::YOLOFY, Action::UNYOLOFY}) {
            const auto input = request(std::string("work-cli --image ") + operand + " --no-confirm describe",
                action, addon::k_runner_powershell, "windows-x86_64");
            const auto result = addon::transform_launcher_approval(input, policy);
            SDK_CHECK(result.disposition == Disposition::UNSUPPORTED && !key(input, policy).supported,
                "PowerShell quote-leading concatenation cannot merge separate native operands");
        }
    }
    // Oracle: both Windows PowerShell 5.1 and PowerShell 7 split bare short
    // parameter attachments at punctuation; quoting keeps one native word.
    for (const auto* operand : {"-ishot.png", "-i=shot.png", "-i=\"shot\".png"}) {
        const auto input = request(std::string("work-cli ") + operand + " --no-confirm describe",
            Action::UNYOLOFY, addon::k_runner_powershell, "windows-x86_64");
        SDK_CHECK(addon::transform_launcher_approval(input, policy).disposition == Disposition::UNSUPPORTED &&
            !key(input, policy).supported,
            "PowerShell short attachments need a complete quoted token or value");
    }
    const auto separated = key(request("work-cli --image shot.png --no-confirm describe"), policy);
    const auto absorbed  = key(request("work-cli --image shot.png describe"), policy);
    SDK_CHECK(separated.supported && absorbed.supported && separated.key != absorbed.key,
        "comparison distinguishes a prompt from another variadic value");

    // Oracle: Qwen's native yargs arrays remain greedy with attached values.
    // Explicit yolo=false equals omission, including configured approval mode;
    // attached yolo=true preserves a following literal true/false prompt.
    policy.canonical_argument = "--approve-all=true";
    policy.bypass_arguments.push_back("--approve-all");
    policy.inactive_bypass_argument = "--approve-all=false";
    policy.flag_arguments_consume_booleans = true;
    policy.value_arguments_consume_options = false;
    policy.variadic_arguments = {{"--tools", false, false}};
    const std::vector<Case> greedy_cases{
        {"work-cli --tools Read Write --no-confirm describe",
            "work-cli --tools Read Write --approve-all=false describe"},
        {"work-cli --tools Read --approval plan describe",
            "work-cli --tools Read --approve-all=true describe", Action::YOLOFY},
        {"work-cli --tools --no-confirm describe", "work-cli --tools --approve-all=false describe"},
        {"work-cli --tools=Read Write --no-confirm describe",
            "work-cli --tools=Read Write --approve-all=false describe"},
        {"work-cli --tools= --no-confirm describe", "work-cli --tools= --approve-all=false describe"},
        {"work-cli false", "work-cli --approve-all=true false", Action::YOLOFY},
        {"work-cli --approve-all=true false", "work-cli false"},
        {"work-cli --tools Read --approve-all=true false", "work-cli --tools Read --approve-all=false false"},
        {"work-cli --tools Read --approve-all=false false",
            "work-cli --tools Read --approve-all=true false", Action::YOLOFY},
        {"work-cli --tools Read --no-confirm -- describe", "work-cli --tools Read -- describe"},
        {"work-cli --tools Read --no-confirm --model fast describe", "work-cli --tools Read --model fast describe"},
    };
    for (const auto& value : greedy_cases) {
        auto input = request(value.before, value.action);
        const auto result = addon::transform_launcher_approval(input, policy);
        SDK_CHECK(result.disposition == Disposition::CHANGED && result.script_body == value.after,
            "native inactive approval spelling preserves greedy-array boundaries: " + value.before);
        const auto before_key = key(input, policy);
        input.launcher.script_body = result.script_body;
        const auto repeated = addon::transform_launcher_approval(input, policy);
        SDK_CHECK(repeated.disposition == Disposition::UNCHANGED && repeated.script_body == value.after &&
            before_key.supported && key(input, policy).supported && before_key.key == key(input, policy).key,
            "greedy-array editing remains idempotent with stable comparison identity");
    }
    SDK_CHECK(key(request("work-cli --tools"), policy).key != key(request("work-cli --tools="), policy).key,
        "a zero-value array differs from one explicit empty value");
    const auto two_boundaries = request("work-cli --tools Read --no-confirm first --tools Write --no-confirm second");
    SDK_CHECK(addon::transform_launcher_approval(two_boundaries, policy).disposition == Disposition::UNSUPPORTED,
        "one canonical bypass cannot preserve two irreducible greedy boundaries");
    return true;
}

} // namespace

bool test_launcher_approval_contracts()
{
    SDK_CHECK(literal_launcher_projection(), "literal launcher projection");
    // Oracle: the adopted launcher action preservation/idempotence contract
    // and launcher-approval@1's closed, bounded wire representation.
    SDK_CHECK(addon::is_valid_operation_name(addon::k_launcher_approval_transform_operation) &&
        addon::is_valid_operation_name(addon::k_launcher_approval_comparison_operation),
        "launcher approval operations pass the core protocol grammar");
    SDK_CHECK(variadic_approval_edits(), "native variadic approval semantics");
    SDK_CHECK(powershell_native_words(), "Windows PowerShell native-word boundary");
    struct Case
    {
        std::string before;
        Action      action;
        std::string after;
        std::string runner = addon::k_runner_direct;
        std::string platform = "linux-x86_64";
    };
    const std::vector<Case> cases{
        {"work-cli", Action::YOLOFY, "work-cli --approve-all"},
        {"work-cli", Action::UNYOLOFY, "work-cli"},
        {"work-cli --no-confirm -y --approve-all", Action::YOLOFY, "work-cli --approve-all"},
        {"work-cli --no-confirm -y --approve-all", Action::UNYOLOFY, "work-cli"},
        {"work-cli --approval bypass", Action::UNYOLOFY, "work-cli"},
        {"work-cli --approval=bypass", Action::YOLOFY, "work-cli --approve-all"},
        {"work-cli --approval plan", Action::UNYOLOFY, "work-cli --approval plan"},
        {"work-cli --approval=plan --confirm", Action::YOLOFY, "work-cli --approve-all"},
        {"work-cli --sandbox unrestricted", Action::UNYOLOFY, "work-cli --sandbox unrestricted"},
        {"work-cli --sandbox read-only", Action::YOLOFY, "work-cli --approve-all"},
        {"work-cli --allow-approve-all --verbose", Action::YOLOFY,
            "work-cli --approve-all --allow-approve-all --verbose"},
        {"work-cli false", Action::YOLOFY, "work-cli --approve-all false"},
        {"work-cli --approve-all false", Action::UNYOLOFY, "work-cli false"},
        {"work-cli --prompt caf\u00e9 --no-confirm", Action::UNYOLOFY, "work-cli --prompt caf\u00e9"},
        {"work-cli --model fast\u00a0--approve-all", Action::UNYOLOFY,
            "work-cli --model fast\u00a0--approve-all", addon::k_runner_bash},
        {"work-cli --model fast\u00a0--approve-all", Action::UNYOLOFY,
            "work-cli --model fast\u00a0--approve-all", addon::k_runner_cmd, "windows-x86_64"},
        {"work-cli --prompt \"--approve-all\" --no-confirm", Action::UNYOLOFY,
            "work-cli --prompt \"--approve-all\""},
        {"work-cli -c approvals=never --settings=--no-confirm --no-confirm", Action::UNYOLOFY,
            "work-cli -c approvals=never --settings=--no-confirm"},
        {"work-cli -- --no-confirm", Action::YOLOFY, "work-cli --approve-all -- --no-confirm"},
        {"work-cli -- --approve-all", Action::UNYOLOFY, "work-cli -- --approve-all"},
        {"work-cli --profile personal resume session-51", Action::YOLOFY,
            "work-cli --profile personal resume --approve-all session-51"},
        {"work-cli --resume session-51 --no-confirm", Action::UNYOLOFY, "work-cli --resume session-51"},
        {"work-cli --resume --no-confirm", Action::UNYOLOFY, "work-cli --resume"},
        {"work-cli --resume --no-confirm -- prompt", Action::UNYOLOFY, "work-cli --resume -- prompt"},
        {"work-cli --resume=session-51 --no-confirm prompt", Action::UNYOLOFY,
            "work-cli --resume=session-51 prompt"},
        {"work-cli --resume session-51 --no-confirm prompt", Action::UNYOLOFY,
            "work-cli --resume session-51 prompt"},
        {"\"/opt/work cli/work-cli\"  --model \"fast model\"", Action::YOLOFY,
            "\"/opt/work cli/work-cli\" --approve-all  --model \"fast model\""},
        {"work-cli --settings '{\"approval\":\"bypass\",\"prompt\":\"--no-confirm\"}' --no-confirm",
            Action::UNYOLOFY, "work-cli --settings '{\"approval\":\"bypass\",\"prompt\":\"--no-confirm\"}'",
            addon::k_runner_bash},
        {"/opt/work\\ cli/work-cli --model 'fast model'", Action::YOLOFY,
            "/opt/work\\ cli/work-cli --approve-all --model 'fast model'", addon::k_runner_bash},
        {"work-cli", Action::YOLOFY, "work-cli --approve-all", addon::k_runner_cmd, "windows-x86_64"},
        {"\"C:\\Tools\\work cli\\work-cli.cmd\" --no-confirm", Action::UNYOLOFY,
            "\"C:\\Tools\\work cli\\work-cli.cmd\"", addon::k_runner_cmd, "windows-x86_64"},
        {"WORK-CLI.EXE --model \"fast model\"", Action::YOLOFY,
            "WORK-CLI.EXE --approve-all --model \"fast model\"", addon::k_runner_cmd, "windows-x86_64"},
        {"work-cli", Action::YOLOFY, "work-cli --approve-all", addon::k_runner_powershell, "windows-x86_64"},
        {"& 'C:\\Tools\\work cli\\work-cli.exe' --profile 'owner''s profile' --no-confirm", Action::UNYOLOFY,
            "& 'C:\\Tools\\work cli\\work-cli.exe' --profile 'owner''s profile'",
            addon::k_runner_powershell, "windows-x86_64"},
    };
    for (const Case& value : cases) {
        auto input = request(value.before, value.action, value.runner, value.platform);
        const auto changed = addon::transform_launcher_approval(input, rules());
        const Disposition expected = value.before == value.after ? Disposition::UNCHANGED : Disposition::CHANGED;
        SDK_CHECK(changed.disposition == expected && changed.script_body == value.after,
            "source-preserving transform: " + value.before + " => " + changed.script_body);
        const auto original_key = key(input);
        input.launcher.script_body = changed.script_body;
        const auto repeated = addon::transform_launcher_approval(input, rules());
        SDK_CHECK(repeated.disposition == Disposition::UNCHANGED && repeated.script_body == value.after,
            "the same action is idempotent: " + value.before);
        SDK_CHECK(original_key.supported && key(input).supported && original_key.key == key(input).key,
            "approval-only editing preserves provider comparison identity: " + value.before);
    }

    // Removing an approval span must not let an optional-value option capture
    // a previously positional word, even when the resulting syntax is valid.
    for (const auto& input : {
            request("work-cli --resume --no-confirm prompt", Action::UNYOLOFY),
            request("work-cli --resume --approval bypass prompt", Action::UNYOLOFY),
            request("work-cli --resume --approval bypass prompt", Action::YOLOFY),
            request("work-cli --resume --approval plan prompt", Action::YOLOFY)})
    {
        const auto result = addon::transform_launcher_approval(input, rules());
        SDK_CHECK(result.disposition == Disposition::UNSUPPORTED && result.script_body.empty(),
            "approval editing preserves non-approval operand ownership");
        SDK_CHECK(key(input).supported, "operand-preservation refusal does not change comparison-key policy");
    }

    const std::vector<addon::Launcher_approval_transform_request> unsupported{
        request("other-cli"), request("work-cli --unknown --no-confirm"),
        request("work-cli --approval future-mode"), request("work-cli --approval"),
        request("work-cli --model -- --approve-all"), request("work-cli --approve-all=false"),
        request("work-cli --settings \"unterminated"), request("work-cli \"\""),
        request("work-cli login"), request("work-cli --profile personal config"),
        request("env WORK_PROFILE=personal work-cli"),
        request("work-cli\nwork-cli", Action::YOLOFY, addon::k_runner_bash),
        request("work-cli && echo ready", Action::YOLOFY, addon::k_runner_bash),
        request("work-cli --prompt $PROMPT", Action::YOLOFY, addon::k_runner_bash),
        request("work-cli --prompt \"$(cat prompt)\"", Action::YOLOFY, addon::k_runner_bash),
        request("work-cli --prompt *.md", Action::YOLOFY, addon::k_runner_bash),
        request("work-cli --profile %PROFILE%", Action::YOLOFY, addon::k_runner_cmd, "windows-x86_64"),
        request("work-cli | more", Action::YOLOFY, addon::k_runner_cmd, "windows-x86_64"),
        request("work-cli --profile $profile", Action::YOLOFY, addon::k_runner_powershell, "windows-x86_64"),
        request("'C:\\Tools\\work-cli.exe'", Action::YOLOFY, addon::k_runner_powershell, "windows-x86_64"),
        request("work-cli", Action::YOLOFY, addon::k_runner_powershell, "linux-x86_64"),
    };
    for (const auto& input : unsupported) {
        const auto result = addon::transform_launcher_approval(input, rules());
        SDK_CHECK(result.disposition == Disposition::UNSUPPORTED && result.script_body.empty() && !key(input).supported,
            "ambiguous invocation is explicitly unsupported: " + input.launcher.script_body);
    }
    // Oracle: Microsoft's native argv rules retain doubled quotes within a
    // quoted string; the CMD argv probe confirmed a literal quoted flag value.
    const std::string cmd_quoted_values[]{
        "\"\"\"--approve-all\"\"\"",
        "\"\"\"--no-confirm\"\"\"",
        "\"{\"\"prompt\"\":\"\"--approve-all\"\"}\"",
    };
    for (const auto& value : cmd_quoted_values) {
        for (const auto action : {Action::YOLOFY, Action::UNYOLOFY}) {
            for (const auto& prefix : {"work-cli ", "work-cli --settings "}) {
                const auto input = request(prefix + value, action, addon::k_runner_cmd, "windows-x86_64");
                const auto result = addon::transform_launcher_approval(input, rules());
                SDK_CHECK(result.disposition == Disposition::UNSUPPORTED && result.script_body.empty() &&
                    !key(input).supported,
                    "literal CMD quote characters cannot turn prompt/settings values into approval options");
            }
        }
    }
    // Oracle: PowerShell about_Quoting_Rules and CharTraits.IsSingleQuote/IsDoubleQuote.
    const std::string smart_quotes[]{"\u2018", "\u2019", "\u201a", "\u201b", "\u201c", "\u201d", "\u201e"};
    for (const auto& quote : smart_quotes) {
        for (const auto action : {Action::YOLOFY, Action::UNYOLOFY}) {
            for (const auto& prefix : {"work-cli ", "work-cli --settings "}) {
                const auto input = request(prefix + quote + "explain --approve-all carefully" + quote,
                    action, addon::k_runner_powershell, "windows-x86_64");
                const auto result = addon::transform_launcher_approval(input, rules());
                SDK_CHECK(result.disposition == Disposition::UNSUPPORTED && result.script_body.empty() &&
                    !key(input).supported,
                    "typographic PowerShell quotes cannot expose prompt/configuration words as approval flags");
            }
        }
    }
    // Oracle: Windows PowerShell 5.1 native argv capture strips embedded double
    // quotes and can expose a bypass option from inside a single quoted operand.
    for (const auto* script : {
            R"(work-cli """--approve-all""")",
            R"(work-cli 'a "b --approve-all c" d')",
            R"(work-cli --settings "{""prompt"":""--approve-all""}" --no-confirm)"})
    {
        for (const auto action : {Action::YOLOFY, Action::UNYOLOFY}) {
            const auto input = request(script, action, addon::k_runner_powershell, "windows-x86_64");
            const auto result = addon::transform_launcher_approval(input, rules());
            SDK_CHECK(result.disposition == Disposition::UNSUPPORTED && result.script_body.empty() &&
                !key(input).supported,
                "PowerShell native quote ambiguity is unsupported for editing and comparison");
        }
    }
    // Oracle: Qt 6.12 QProcess::splitCommand uses QChar::isSpace; Windows
    // PowerShell 5.1 native argv capture also separates these Unicode spaces.
    const std::string unicode_spaces[]{
        "\u0085", "\u00a0", "\u1680", "\u2000", "\u2001", "\u2002", "\u2003", "\u2004",
        "\u2005", "\u2006", "\u2007", "\u2008", "\u2009", "\u200a", "\u2028", "\u2029",
        "\u202f", "\u205f", "\u3000",
    };
    for (const auto& separator : unicode_spaces) {
        for (const auto* runner : {addon::k_runner_direct, addon::k_runner_powershell}) {
            for (const auto action : {Action::YOLOFY, Action::UNYOLOFY}) {
                const auto input = request("work-cli --model fast" + separator + "--approve-all",
                    action, runner, "windows-x86_64");
                const auto result = addon::transform_launcher_approval(input, rules());
                SDK_CHECK(result.disposition == Disposition::UNSUPPORTED && result.script_body.empty() &&
                    !key(input).supported,
                    "Unicode runner separators cannot hide approval options inside values");
            }
        }
    }
    const auto maximum = request("work-cli " + std::string(addon::limits::k_max_script_body_bytes - 9, 'p'));
    SDK_CHECK(addon::transform_launcher_approval(maximum, rules()).disposition == Disposition::UNSUPPORTED,
        "a transform cannot exceed the saved script bound");

    const auto base = request("work-cli");
    std::vector<addon::Launcher_approval_transform_request> distinct{
        request("/opt/bin/work-cli"), request("work-cli --profile personal"),
        request("work-cli --model fast"), request("work-cli --settings approvals=never"),
        request("work-cli -- --approve-all"), request("work-cli", Action::YOLOFY, addon::k_runner_bash),
        request("work-cli", Action::YOLOFY, addon::k_runner_direct, "macos-arm64"),
    };
    distinct.push_back(base);
    distinct.back().launcher.environment_text += "\nWORK_CONFIG=/home/owner/custom";
    for (const auto& input : distinct) {
        SDK_CHECK(key(input).supported && key(input).key != key(base).key,
            "comparison preserves non-approval arguments, executable, runner, platform and environment");
    }
    for (const auto& values : std::vector<std::pair<std::string, std::string>>{
            {"work-cli --resume --no-confirm prompt", "work-cli --resume prompt"},
            {"work-cli --resume", "work-cli --resume="},
            {"work-cli --settings --confirm", "work-cli --settings=--confirm"}})
    {
        const auto left  = key(request(values.first));
        const auto right = key(request(values.second));
        SDK_CHECK(left.supported && right.supported && left.key != right.key,
            "comparison retains optional ownership and scalar attachment distinctions");
    }

    addon::Contract_failure failure;
    const auto input = request("work-cli --no-confirm", Action::UNYOLOFY);
    const auto decoded = addon::decode_launcher_approval_transform_request(
        addon::encode_launcher_approval_transform_request(input), &failure);
    SDK_CHECK(decoded && decoded->action == input.action && decoded->platform_id == input.platform_id &&
        decoded->launcher.script_body == input.launcher.script_body &&
        decoded->launcher.environment_text == input.launcher.environment_text,
        "the wire carries the raw saved tuple and action");
    auto invalid = addon::encode_launcher_approval_transform_request(input);
    invalid.set("action", addon::Json_value("toggle"));
    SDK_CHECK(!addon::decode_launcher_approval_transform_request(invalid, &failure), "unknown action is invalid");
    invalid = addon::encode_launcher_approval_transform_request(input);
    invalid.set("argv", addon::Json_value::array());
    SDK_CHECK(!addon::decode_launcher_approval_transform_request(invalid, &failure),
        "the request does not accept prepared argv");
    for (const auto disposition : {Disposition::CHANGED, Disposition::UNCHANGED, Disposition::UNSUPPORTED}) {
        const addon::Launcher_approval_transform_result result{disposition,
            disposition == Disposition::UNSUPPORTED ? std::string{} : "work-cli"};
        const auto round_trip = addon::decode_launcher_approval_transform_result(
            addon::encode_launcher_approval_transform_result(result), &failure);
        SDK_CHECK(round_trip && round_trip->disposition == disposition && round_trip->script_body == result.script_body,
            "each transform disposition has one result shape");
    }
    invalid = addon::encode_launcher_approval_transform_result({});
    invalid.set("script_body", addon::Json_value("work-cli"));
    SDK_CHECK(!addon::decode_launcher_approval_transform_result(invalid, &failure),
        "an unsupported transform cannot carry a replacement script");
    const auto comparison = addon::decode_launcher_approval_comparison_request(
        addon::encode_launcher_approval_comparison_request({input.launcher, input.platform_id}), &failure);
    SDK_CHECK(comparison && comparison->launcher.script_body == input.launcher.script_body,
        "comparison uses the same saved tuple");
    for (const auto& result : {key(base), addon::Launcher_approval_comparison_result{}}) {
        const auto round_trip = addon::decode_launcher_approval_comparison_result(
            addon::encode_launcher_approval_comparison_result(result), &failure);
        SDK_CHECK(round_trip && round_trip->supported == result.supported && round_trip->key == result.key,
            "comparison distinguishes a supported key from an unsupported invocation");
    }
    invalid = addon::encode_launcher_approval_comparison_result(
        {true, std::string(addon::k_max_launcher_approval_key_bytes + 1, 'k')});
    SDK_CHECK(!addon::decode_launcher_approval_comparison_result(invalid, &failure), "comparison keys are bounded");
    SDK_CHECK(addon::validate_launcher_approval_declaration(addon::Json_value::object(), &failure),
        "the sidecar declares the optional capability without a common schema change");
    invalid = addon::Json_value::object();
    invalid.set("arguments", addon::Json_value::array());
    SDK_CHECK(!addon::validate_launcher_approval_declaration(invalid, &failure),
        "provider policy is not a host-owned sidecar argument table");
    return true;
}

} // namespace sdk_tests
