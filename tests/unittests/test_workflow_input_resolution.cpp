#include "../../app/workflow/workflow.h"

#include "engine/framework/io/json.h"
#include "engine/framework/runtime/registry.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using Entries = std::vector<std::pair<std::string, std::string>>;

void require(bool condition, const std::string & message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void require_equal(const std::string & actual, const std::string & expected, const std::string & what) {
    if (actual != expected) {
        throw std::runtime_error(what + ": expected '" + expected + "', got '" + actual + "'");
    }
}

// Owns one freshly created temp directory and removes only that directory.
struct TempRoot {
    std::filesystem::path path;

    TempRoot() {
        std::random_device rd;
        const auto base = std::filesystem::absolute(std::filesystem::temp_directory_path());
        for (int attempt = 0; attempt < 16; ++attempt) {
            const auto candidate = base /
                ("audio_cpp_workflow_input_resolution_test_" + std::to_string(rd()) + "_" + std::to_string(rd()));
            // Refuse a collision instead of deleting or reusing another test's files.
            if (std::filesystem::create_directory(candidate)) {
                path = candidate;
                return;
            }
        }
        throw std::runtime_error("could not create isolated temp directory");
    }

    ~TempRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    TempRoot(const TempRoot &) = delete;
    TempRoot & operator=(const TempRoot &) = delete;
};

void write_file(const std::filesystem::path & path, const std::string & content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    require(out.good(), "failed to create test file " + path.string());
    out << content;
}

std::string json_string(const std::string & value) {
    return engine::io::json::stringify_string(value);
}

std::string json_object(const Entries & entries) {
    std::string out = "{";
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i != 0) {
            out += ", ";
        }
        out += json_string(entries[i].first) + ": " + json_string(entries[i].second);
    }
    out += "}";
    return out;
}

struct WorkflowRun {
    std::filesystem::path workflow_dir;
    std::filesystem::path output_dir;
    engine::io::json::Value manifest;

    std::string value(const std::string & key) const {
        const auto * found = manifest.find(key);
        require(found != nullptr && found->is_string(), "workflow manifest missing key: " + key);
        return found->as_string();
    }
};

minitts::app::WorkflowRunOptions make_options(
    const std::filesystem::path & root,
    const Entries & inputs,
    const std::string & steps_json,
    const Entries & cli_inputs) {
    const auto workflow_path = root / "wf" / "workflow.json";
    write_file(workflow_path, "{\n  \"inputs\": " + json_object(inputs) + ",\n  \"steps\": " + steps_json + "\n}\n");
    minitts::app::WorkflowRunOptions options;
    options.workflow_path = workflow_path;
    options.output_dir = root / "out";
    for (const auto & [key, value] : cli_inputs) {
        options.workflow_inputs[key] = value;
    }
    return options;
}

WorkflowRun run_workflow(
    const std::filesystem::path & root,
    const Entries & inputs,
    const std::string & steps_json = "[]",
    const Entries & cli_inputs = {}) {
    const auto options = make_options(root, inputs, steps_json, cli_inputs);
    engine::runtime::ModelRegistry registry{};
    minitts::app::run_json_workflow(registry, options);
    WorkflowRun run;
    run.workflow_dir = root / "wf";
    run.output_dir = root / "out";
    run.manifest = engine::io::json::parse_file(run.output_dir / "workflow_manifest.json");
    require(run.manifest.is_object(), "workflow manifest must be an object");
    return run;
}

void require_throws_containing(
    const std::function<void()> & fn,
    const std::vector<std::string> & fragments,
    const std::string & what) {
    try {
        fn();
    } catch (const std::runtime_error & error) {
        const std::string message = error.what();
        for (const auto & fragment : fragments) {
            require(message.find(fragment) != std::string::npos,
                    what + ": error '" + message + "' does not mention '" + fragment + "'");
        }
        return;
    }
    throw std::runtime_error(what + ": expected an error");
}

// A three-input chain root -> mid -> leaf, declared in every textual order and
// under several key spellings, must always resolve fully and must be usable as
// a real batch_inputs text_dir.
void test_three_key_chain_in_every_declaration_order() {
    const std::array<std::array<std::string, 3>, 4> name_sets{{
        {"a", "b", "c"},
        {"c", "b", "a"},
        {"zz_root", "mm_mid", "aa_leaf"},
        {"leaf_dir_root", "x", "data_root"},
    }};
    for (const auto & names : name_sets) {
        const std::string & root_key = names[0];
        const std::string & mid_key = names[1];
        const std::string & leaf_key = names[2];
        std::array<Entries::value_type, 3> entries{{
            {root_key, "${workflow_dir}/in"},
            {mid_key, "${" + root_key + "}/data"},
            {leaf_key, "${" + mid_key + "}/texts"},
        }};
        std::array<int, 3> order{0, 1, 2};
        do {
            TempRoot temp;
            const auto texts_dir = temp.path / "wf" / "in" / "data" / "texts";
            write_file(texts_dir / "one.txt", "first  item\n");
            write_file(texts_dir / "two.md", "second item");
            write_file(texts_dir / "ignored.bin", "not a text input");

            Entries inputs;
            std::string label = "order";
            for (int index : order) {
                inputs.push_back(entries[static_cast<size_t>(index)]);
                label += " " + entries[static_cast<size_t>(index)].first;
            }
            const std::string steps =
                "[{\"id\": \"texts\", \"type\": \"batch_inputs\", \"text_dir\": \"${" + leaf_key + "}\"}]";
            const auto run = run_workflow(temp.path, inputs, steps);

            const std::string root_value = run.workflow_dir.string() + "/in";
            require_equal(run.value(root_key), root_value, label + ": root input");
            require_equal(run.value(mid_key), root_value + "/data", label + ": middle input");
            require_equal(run.value(leaf_key), root_value + "/data/texts", label + ": leaf input");
            require_equal(run.value("texts.count"), "2", label + ": batch_inputs traversal of expanded text_dir");
        } while (std::next_permutation(order.begin(), order.end()));
    }
}

// CLI overrides replace defaults before any expansion, so dependents of an
// overridden key see the override, CLI-only keys can reference defaults, and an
// override can break a cycle that only exists among the defaults.
void test_cli_overrides_feed_dependents() {
    TempRoot temp;
    const auto run = run_workflow(
        temp.path,
        {
            {"leaf", "${mid}/leaf"},
            {"mid", "${base}/sub"},
            {"base", "${workflow_dir}/default"},
            {"untouched", "${workflow_dir}/kept"},
            {"loop_a", "${loop_b}"},
            {"loop_b", "${loop_a}"},
        },
        "[]",
        {
            {"base", "${out_dir}/cli"},
            {"cli_only", "${leaf}!"},
            {"loop_a", "fixed"},
        });
    const std::string base = run.output_dir.string() + "/cli";
    require_equal(run.value("base"), base, "CLI override of base");
    require_equal(run.value("mid"), base + "/sub", "default depending on CLI override");
    require_equal(run.value("leaf"), base + "/sub/leaf", "second-level dependent of CLI override");
    require_equal(run.value("cli_only"), base + "/sub/leaf!", "CLI-only input referencing defaults");
    require_equal(run.value("untouched"), run.workflow_dir.string() + "/kept", "default without override");
    require_equal(run.value("loop_a"), "fixed", "CLI override replacing a cyclic default");
    require_equal(run.value("loop_b"), "fixed", "dependent of cycle-breaking override");
}

// Names that are not inputs are left for later steps; builtins win over inputs
// that happen to share their name; repeated references expand every occurrence.
void test_unknown_placeholders_and_builtins() {
    TempRoot temp;
    const auto run = run_workflow(
        temp.path,
        {
            {"later", "${transcribe.text_path}"},
            {"item_ref", "${item.id}/${plain}"},
            {"open", "${plain}/${unterminated"},
            {"money", "cost $5 {not} $plain"},
            {"plain", "literal"},
            {"twice", "${plain}-${plain}"},
            {"fanout", "${twice}+${twice}"},
            {"workflow_dir", "/not/the/builtin"},
            {"uses_workflow_dir", "${workflow_dir}/x"},
            {"uses_out_dir", "${out_dir}/y"},
        });
    require_equal(run.value("later"), "${transcribe.text_path}", "unknown step output placeholder");
    require_equal(run.value("item_ref"), "${item.id}/literal", "unknown foreach placeholder beside known input");
    require_equal(run.value("open"), "literal/${unterminated", "unterminated placeholder");
    require_equal(run.value("money"), "cost $5 {not} $plain", "text without placeholders");
    require_equal(run.value("twice"), "literal-literal", "repeated reference");
    require_equal(run.value("fanout"), "literal-literal+literal-literal", "fan-out reference");
    require_equal(run.value("workflow_dir"), "/not/the/builtin", "input sharing a builtin name is stored as given");
    require_equal(run.value("uses_workflow_dir"), run.workflow_dir.string() + "/x", "builtin workflow_dir precedence");
    require_equal(run.value("uses_out_dir"), run.output_dir.string() + "/y", "builtin out_dir precedence");
}

void test_reference_cycles_are_rejected() {
    // A step that would fail differently proves the error comes from input
    // resolution, before any step runs or the output directory is created.
    const std::string steps =
        "[{\"id\": \"texts\", \"type\": \"batch_inputs\", \"text_dir\": \"${workflow_dir}/missing\"}]";
    struct Case {
        std::string name;
        Entries inputs;
        Entries cli_inputs;
        std::vector<std::string> fragments;
    };
    const std::vector<Case> cases{
        {"self cycle", {{"a", "${a}/x"}, {"ok", "fine"}}, {}, {"cycle", "a -> a"}},
        {"indirect cycle",
         {{"c", "${a}/3"}, {"a", "${b}/1"}, {"b", "${c}/2"}, {"ok", "${workflow_dir}"}},
         {},
         {"cycle", "a -> b -> c -> a"}},
        {"cycle behind a valid prefix",
         {{"head", "${loop1}"}, {"loop1", "${loop2}"}, {"loop2", "${loop1}"}},
         {},
         {"cycle", "loop1 -> loop2 -> loop1"}},
        {"cycle introduced by CLI override", {{"a", "plain"}, {"b", "${a}"}}, {{"a", "${b}"}}, {"cycle", "a -> b -> a"}},
    };
    for (const auto & test_case : cases) {
        TempRoot temp;
        const auto options = make_options(temp.path, test_case.inputs, steps, test_case.cli_inputs);
        engine::runtime::ModelRegistry registry{};
        require_throws_containing(
            [&]() { minitts::app::run_json_workflow(registry, options); },
            test_case.fragments,
            test_case.name);
        require(!std::filesystem::exists(temp.path / "out"),
                test_case.name + ": output directory must not be created");
    }
}

}  // namespace

int main() {
    const std::vector<std::pair<const char *, void (*)()>> cases{
        {"test_three_key_chain_in_every_declaration_order", test_three_key_chain_in_every_declaration_order},
        {"test_cli_overrides_feed_dependents", test_cli_overrides_feed_dependents},
        {"test_unknown_placeholders_and_builtins", test_unknown_placeholders_and_builtins},
        {"test_reference_cycles_are_rejected", test_reference_cycles_are_rejected}
    };
    size_t failed = 0;
    for (const auto & test_case : cases) {
        try {
            test_case.second();
            std::cout << "PASS " << test_case.first << '\n';
        } catch (const std::exception & error) {
            ++failed;
            std::cerr << "FAIL " << test_case.first << ": " << error.what() << '\n';
        }
    }
    std::cout << (cases.size() - failed) << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
