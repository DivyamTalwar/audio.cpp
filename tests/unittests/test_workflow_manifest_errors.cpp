#include "../../app/workflow/workflow.h"
#include "engine/framework/io/json.h"
#include "engine/framework/runtime/registry.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
void require(bool ok, const std::string & message) {
    if (!ok) throw std::runtime_error(message);
}
struct TempRoot {
    fs::path path;
    TempRoot() {
        std::random_device random;
        for (int i = 0; i < 32; ++i) {
            const auto candidate = fs::temp_directory_path() /
                ("audio-workflow-manifest-" + std::to_string(random()) + "-" + std::to_string(random()));
            if (fs::create_directory(candidate)) { path = candidate; return; }
        }
        throw std::runtime_error("cannot create owned temporary directory");
    }
    ~TempRoot() { std::error_code ignored; fs::remove_all(path, ignored); }
    TempRoot(const TempRoot &) = delete;
    TempRoot & operator=(const TempRoot &) = delete;
};
struct CaptureOutput {
    std::ostringstream output;
    std::streambuf * previous = std::cout.rdbuf(output.rdbuf());
    ~CaptureOutput() { std::cout.rdbuf(previous); }
};
void write_fixture(const fs::path & path, const std::string & text) {
    std::ofstream stream(path, std::ios::binary);
    stream << text;
    stream.close();
    require(!stream.fail(), "could not write fixture " + path.string());
}
minitts::app::WorkflowRunOptions options_for(const TempRoot & root) {
    minitts::app::WorkflowRunOptions options;
    options.workflow_path = root.path / "workflow.json";
    options.output_dir = root.path / "out";
    write_fixture(options.workflow_path, R"({"inputs":{"label":"line one\nline two"},"steps":[]})");
    return options;
}
void run(const minitts::app::WorkflowRunOptions & options) {
    engine::runtime::ModelRegistry registry;
    minitts::app::run_json_workflow(registry, options);
}
void verify_success(bool replace_existing) {
    TempRoot root;
    const auto options = options_for(root);
    fs::create_directories(options.output_dir);
    const auto manifest = options.output_dir / "workflow_manifest.json";
    if (replace_existing) write_fixture(manifest, std::string(2048, 'x'));
    CaptureOutput capture;
    run(options);
    const auto parsed = engine::io::json::parse_file(manifest);
    const auto * label = parsed.find("label");
    require(label && label->as_string() == "line one\nline two", "manifest contents did not round-trip");
    require(capture.output.str().find("workflow_manifest_out=" + manifest.string()) != std::string::npos,
            "successful publication was not reported");
}
void expect_manifest_failure(const minitts::app::WorkflowRunOptions & options, const std::string & phase) {
    CaptureOutput capture;
    bool threw = false;
    try { run(options); }
    catch (const std::runtime_error & error) {
        threw = true;
        const std::string message(error.what());
        require(message.find(phase) != std::string::npos, "error did not identify the failure phase: " + message);
        require(message.find("workflow_manifest.json") != std::string::npos, "error did not identify the manifest");
    }
    require(threw, "workflow accepted a failed manifest write");
    require(capture.output.str().find("workflow_manifest_out=") == std::string::npos,
            "failed publication printed a success marker");
}
void test_open_error() {
    TempRoot root;
    const auto options = options_for(root);
    const auto manifest = options.output_dir / "workflow_manifest.json";
    fs::create_directories(manifest);
    write_fixture(manifest / "keep.txt", "keep this directory intact");
    expect_manifest_failure(options, "open");
    require(fs::is_directory(manifest), "existing destination directory was replaced");
    std::ifstream input(manifest / "keep.txt");
    std::stringstream text; text << input.rdbuf();
    require(text.str() == "keep this directory intact", "existing directory contents changed");
}
#if defined(__linux__)
void test_close_error() {
    TempRoot root;
    const auto options = options_for(root);
    fs::create_directories(options.output_dir);
    // /dev/full is a Linux error device: open succeeds; every write fails.
    // Only this test-owned symlink is removed during cleanup.
    fs::create_symlink("/dev/full", options.output_dir / "workflow_manifest.json");
    expect_manifest_failure(options, "write");
}
#endif
}  // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> cases{
        {"manifest_create", [] { verify_success(false); }},
        {"manifest_replace", [] { verify_success(true); }},
        {"manifest_open_error", test_open_error},
#if defined(__linux__)
        {"manifest_close_error", test_close_error},
#endif
    };
    size_t failed = 0;
    for (const auto & item : cases) {
        try { item.second(); std::cout << "PASS " << item.first << '\n'; }
        catch (const std::exception & error) {
            ++failed; std::cerr << "FAIL " << item.first << ": " << error.what() << '\n';
        }
    }
    std::cout << cases.size() - failed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
