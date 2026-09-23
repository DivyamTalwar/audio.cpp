#include "../../app/workflow/workflow.h"

#include "engine/framework/audio/output.h"
#include "engine/framework/audio/wav_reader.h"
#include "engine/framework/io/json.h"
#include "engine/framework/runtime/registry.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

// Runs chunked_model steps through run_json_workflow with an in-process fake
// model. The fake ASR session "transcribes" each chunk as L<n>, where n is the
// chunk's mean sample level scaled by 8, so every item and chunk produces
// distinct text without weights or inference.

namespace {

constexpr int kSampleRate = 8000;
constexpr int kChunkFrames = 2000;  // chunk_seconds = 0.25

void require(bool condition, const std::string & message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string read_file(const std::filesystem::path & path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "failed to open test output " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_file(const std::filesystem::path & path, const std::string & text) {
    std::ofstream out(path, std::ios::binary);
    require(out.good(), "failed to write test input " + path.string());
    out << text;
}

void require_file_text(const std::filesystem::path & path, const std::string & expected) {
    require(std::filesystem::is_regular_file(path), "missing expected file " + path.string());
    const std::string actual = read_file(path);
    require(actual == expected,
            "unexpected content in " + path.string() + ": expected [" + expected + "] actual [" + actual + "]");
}

long level_of(const std::vector<float> & samples) {
    double sum = 0.0;
    for (float sample : samples) {
        sum += sample;
    }
    return std::lround(sum / static_cast<double>(samples.size()) * 8.0);
}

// Two constant-level halves, so a 0.25 s chunk size yields exactly two chunks.
void write_two_level_wav(const std::filesystem::path & path, float first, float second) {
    std::vector<float> samples(static_cast<size_t>(kChunkFrames), first);
    samples.insert(samples.end(), static_cast<size_t>(kChunkFrames), second);
    engine::audio::WavPcm16Sink().write(path, engine::audio::AudioBuffer{kSampleRate, 1, samples});
}

void require_chunk_level(const std::filesystem::path & path, long expected) {
    require(std::filesystem::is_regular_file(path), "missing retained chunk " + path.string());
    const auto wav = engine::audio::read_wav_f32(path);
    require(wav.samples.size() == static_cast<size_t>(kChunkFrames), "unexpected chunk length " + path.string());
    const long actual = level_of(wav.samples);
    require(actual == expected,
            "chunk " + path.string() + " has level " + std::to_string(actual) +
                ", expected " + std::to_string(expected));
}

std::string trim_trailing_whitespace(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}

struct EchoLog {
    std::vector<std::string> transcript_paths;
};

class FakeSession final : public engine::runtime::IOfflineVoiceTaskSession {
public:
    FakeSession(engine::runtime::VoiceTaskKind task, std::shared_ptr<EchoLog> log)
        : task_(task), log_(std::move(log)) {}

    std::string family() const override { return "fake_workflow"; }
    engine::runtime::VoiceTaskKind task_kind() const override { return task_; }
    engine::runtime::RunMode run_mode() const override { return engine::runtime::RunMode::Offline; }
    void prepare(const engine::runtime::SessionPreparationRequest &) override {}

    engine::runtime::TaskResult run(const engine::runtime::TaskRequest & request) override {
        engine::runtime::TaskResult result;
        if (task_ == engine::runtime::VoiceTaskKind::Asr) {
            require(request.audio_input.has_value() && !request.audio_input->samples.empty(),
                    "fake asr requires chunk audio");
            result.text_output = engine::runtime::Transcript{
                "L" + std::to_string(level_of(request.audio_input->samples)), "en"};
            return result;
        }
        // Downstream consumer: read the upstream transcript through its path.
        const auto it = request.options.find("transcript_path");
        require(it != request.options.end(), "fake echo requires transcript_path");
        log_->transcript_paths.push_back(it->second);
        result.text_output = engine::runtime::Transcript{
            "echo:" + trim_trailing_whitespace(read_file(it->second)), "en"};
        return result;
    }

private:
    engine::runtime::VoiceTaskKind task_;
    std::shared_ptr<EchoLog> log_;
};

class FakeModel final : public engine::runtime::ILoadedVoiceModel {
public:
    explicit FakeModel(std::shared_ptr<EchoLog> log) : log_(std::move(log)) {
        metadata_.family = "fake_workflow";
    }

    const engine::runtime::ModelMetadata & metadata() const noexcept override { return metadata_; }
    const engine::runtime::CapabilitySet & capabilities() const noexcept override { return capabilities_; }
    std::unique_ptr<engine::runtime::IVoiceTaskSession> create_task_session(
        const engine::runtime::TaskSpec & task,
        const engine::runtime::SessionOptions &) const override {
        return std::make_unique<FakeSession>(task.task, log_);
    }

private:
    engine::runtime::ModelMetadata metadata_;
    engine::runtime::CapabilitySet capabilities_;
    std::shared_ptr<EchoLog> log_;
};

class FakeLoader final : public engine::runtime::IVoiceModelLoader {
public:
    explicit FakeLoader(std::shared_ptr<EchoLog> log) : log_(std::move(log)) {}

    std::string family() const override { return "fake_workflow"; }
    bool can_load(const engine::runtime::ModelLoadRequest &) const override { return true; }
    engine::runtime::ModelInspection inspect(const engine::runtime::ModelLoadRequest &) const override {
        engine::runtime::ModelInspection inspection;
        inspection.metadata.family = "fake_workflow";
        return inspection;
    }
    std::unique_ptr<engine::runtime::ILoadedVoiceModel> load(
        const engine::runtime::ModelLoadRequest &) const override {
        return std::make_unique<FakeModel>(log_);
    }

private:
    std::shared_ptr<EchoLog> log_;
};

const char * kWorkflowJson = R"JSON({
  "steps": [
    {"id": "clips", "type": "batch_inputs", "audio_dir": "${workflow_dir}/clips"},
    {
      "id": "asr",
      "type": "chunked_model",
      "foreach": "${clips.items}",
      "model": "${workflow_dir}/fake_model.bin",
      "family": "fake_workflow",
      "task": "asr",
      "chunk_seconds": 0.25,
      "request": {"audio": "${item.audio_path}"}
    },
    {
      "id": "echo",
      "type": "model",
      "foreach": "${asr.items}",
      "model": "${workflow_dir}/fake_model.bin",
      "family": "fake_workflow",
      "task": "tts",
      "request": {"options": {"transcript_path": "${item.text_path}"}},
      "text_out": "${out_dir}/echo/${item.id}.txt"
    },
    {
      "id": "custom",
      "type": "chunked_model",
      "foreach": "${clips.items}",
      "model": "${workflow_dir}/fake_model.bin",
      "family": "fake_workflow",
      "task": "asr",
      "chunk_seconds": 0.25,
      "request": {"audio": "${item.audio_path}"},
      "text_out": "${out_dir}/custom_text/${item.id}.txt"
    },
    {
      "id": "solo",
      "type": "chunked_model",
      "model": "${workflow_dir}/fake_model.bin",
      "family": "fake_workflow",
      "task": "asr",
      "chunk_seconds": 0.25,
      "request": {"audio": "${workflow_dir}/solo.wav"}
    }
  ]
}
)JSON";

void test_chunked_foreach_items_keep_separate_artifacts() {
    const auto root = std::filesystem::temp_directory_path() /
        ("audio_cpp_workflow_chunked_foreach_test_" + std::to_string(std::random_device{}()));
    // Refuse a collision instead of deleting or reusing another test's files.
    require(std::filesystem::create_directory(root), "could not create isolated output directory");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    } cleanup{root};

    const auto workflow_dir = root / "workflow";
    const auto out = root / "out";
    std::filesystem::create_directories(workflow_dir / "clips");
    write_file(workflow_dir / "fake_model.bin", "fake");
    write_two_level_wav(workflow_dir / "clips" / "alpha.wav", 0.25F, 0.5F);     // L2 L4
    write_two_level_wav(workflow_dir / "clips" / "beta.wav", -0.25F, -0.625F);  // L-2 L-5
    write_two_level_wav(workflow_dir / "solo.wav", 0.75F, 0.125F);              // L6 L1
    write_file(workflow_dir / "workflow.json", kWorkflowJson);

    auto log = std::make_shared<EchoLog>();
    engine::runtime::ModelRegistry registry;
    registry.register_loader(std::make_shared<FakeLoader>(log));

    minitts::app::WorkflowRunOptions options;
    options.workflow_path = workflow_dir / "workflow.json";
    options.output_dir = out;
    minitts::app::run_json_workflow(registry, options);

    // foreach: each item keeps its own default transcript and retained chunks.
    require_file_text(out / "asr" / "alpha" / "asr.txt", "L2 L4\n");
    require_file_text(out / "asr" / "beta" / "asr.txt", "L-2 L-5\n");
    require_chunk_level(out / "asr" / "alpha" / "chunks" / "chunk_0.wav", 2);
    require_chunk_level(out / "asr" / "alpha" / "chunks" / "chunk_1.wav", 4);
    require_chunk_level(out / "asr" / "beta" / "chunks" / "chunk_0.wav", -2);
    require_chunk_level(out / "asr" / "beta" / "chunks" / "chunk_1.wav", -5);
    require(!std::filesystem::exists(out / "asr" / "asr.txt"),
            "foreach chunked_model must not write a shared default transcript");
    require(!std::filesystem::exists(out / "asr" / "chunks"),
            "foreach chunked_model must not use a shared chunk directory");

    // Downstream foreach receives each item's own text_path and reads it.
    require(log->transcript_paths.size() == 2, "echo step should run once per asr item");
    require(std::filesystem::path(log->transcript_paths[0]) == out / "asr" / "alpha" / "asr.txt",
            "alpha item text_path mismatch: " + log->transcript_paths[0]);
    require(std::filesystem::path(log->transcript_paths[1]) == out / "asr" / "beta" / "asr.txt",
            "beta item text_path mismatch: " + log->transcript_paths[1]);
    require_file_text(out / "echo" / "alpha.txt", "echo:L2 L4\n");
    require_file_text(out / "echo" / "beta.txt", "echo:L-2 L-5\n");

    // Explicit text_out in foreach is honored as written; chunks stay per item.
    require_file_text(out / "custom_text" / "alpha.txt", "L2 L4\n");
    require_file_text(out / "custom_text" / "beta.txt", "L-2 L-5\n");
    require(!std::filesystem::exists(out / "custom" / "alpha" / "custom.txt"),
            "explicit text_out must replace the default transcript path");
    require_chunk_level(out / "custom" / "alpha" / "chunks" / "chunk_1.wav", 4);
    require_chunk_level(out / "custom" / "beta" / "chunks" / "chunk_1.wav", -5);

    // Non-foreach layout is unchanged: <out>/<id>/chunks and <out>/<id>/<id>.txt.
    require_file_text(out / "solo" / "solo.txt", "L6 L1\n");
    require_chunk_level(out / "solo" / "chunks" / "chunk_0.wav", 6);
    require_chunk_level(out / "solo" / "chunks" / "chunk_1.wav", 1);
    const auto manifest = engine::io::json::parse_file(out / "workflow_manifest.json");
    require(std::filesystem::path(manifest.require("solo.text_path").as_string()) == out / "solo" / "solo.txt",
            "non-foreach solo.text_path must keep the step-level default");
    require(manifest.require("solo.text").as_string() == "L6 L1", "non-foreach solo.text mismatch");
    require(manifest.require("asr.count").as_string() == "2", "asr batch count mismatch");
}

}  // namespace

int main() {
    try {
        test_chunked_foreach_items_keep_separate_artifacts();
    } catch (const std::exception & error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "workflow_chunked_foreach_paths_test passed\n";
    return 0;
}
