// Workflow path bookkeeping for model steps that return named audio outputs.
//
// A model that returns exactly one named output and no primary audio_output
// still has that output written to the step's audio_out by emit_task_result.
// The workflow must publish that file as <step>.audio_path next to the
// <step>.<name>_path alias so downstream steps can consume either. The model
// here is an in-process fake that returns tiny synthetic buffers; nothing is
// loaded from disk and no inference runs.

#include "../../app/workflow/workflow.h"

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
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace rt = engine::runtime;

constexpr const char * kFamily = "fake_named_audio";
constexpr int kSampleRate = 8000;
constexpr size_t kFrames = 32;

void require(bool condition, const std::string & message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

float amplitude_for(const std::string & word) {
    if (word == "alpha") {
        return 0.25F;
    }
    if (word == "beta") {
        return 0.5F;
    }
    if (word == "gamma") {
        return 0.75F;
    }
    throw std::runtime_error("fake model received unexpected text: " + word);
}

std::vector<float> synthetic_samples(float amplitude) {
    static const float pattern[4] = {0.0F, 1.0F, -1.0F, 0.5F};
    std::vector<float> samples(kFrames);
    for (size_t i = 0; i < kFrames; ++i) {
        samples[i] = amplitude * pattern[i % 4];
    }
    return samples;
}

rt::AudioBuffer synthetic_audio(float amplitude) {
    rt::AudioBuffer audio;
    audio.sample_rate = kSampleRate;
    audio.channels = 1;
    audio.samples = synthetic_samples(amplitude);
    return audio;
}

// Text protocol for the fake session:
//   "primary:<word>" -> audio_output only
//   "multi:<word>"   -> named "vocals" and "accompaniment" (half amplitude)
//   "<word>"         -> a single named "vocals" output
class FakeSession final : public rt::IOfflineVoiceTaskSession {
public:
    std::string family() const override { return kFamily; }
    rt::VoiceTaskKind task_kind() const override { return rt::VoiceTaskKind::Tts; }
    rt::RunMode run_mode() const override { return rt::RunMode::Offline; }
    void prepare(const rt::SessionPreparationRequest &) override {}

    rt::TaskResult run(const rt::TaskRequest & request) override {
        require(request.text_input.has_value(), "fake model requires text input");
        const std::string & text = request.text_input->text;
        rt::TaskResult result;
        const std::string primary = "primary:";
        const std::string multi = "multi:";
        if (text.rfind(primary, 0) == 0) {
            result.audio_output = synthetic_audio(amplitude_for(text.substr(primary.size())));
        } else if (text.rfind(multi, 0) == 0) {
            const float amplitude = amplitude_for(text.substr(multi.size()));
            result.named_audio_outputs.push_back({"vocals", synthetic_audio(amplitude), {}});
            result.named_audio_outputs.push_back({"accompaniment", synthetic_audio(amplitude * 0.5F), {}});
        } else {
            result.named_audio_outputs.push_back({"vocals", synthetic_audio(amplitude_for(text)), {}});
        }
        return result;
    }
};

class FakeModel final : public rt::ILoadedVoiceModel {
public:
    FakeModel() {
        metadata_.family = kFamily;
        capabilities_.supported_tasks.push_back({rt::VoiceTaskKind::Tts, {rt::RunMode::Offline}});
    }
    const rt::ModelMetadata & metadata() const noexcept override { return metadata_; }
    const rt::CapabilitySet & capabilities() const noexcept override { return capabilities_; }
    std::unique_ptr<rt::IVoiceTaskSession> create_task_session(
        const rt::TaskSpec & task,
        const rt::SessionOptions &) const override {
        require(task.task == rt::VoiceTaskKind::Tts, "fake model only supports tts");
        return std::make_unique<FakeSession>();
    }

private:
    rt::ModelMetadata metadata_;
    rt::CapabilitySet capabilities_;
};

class FakeLoader final : public rt::IVoiceModelLoader {
public:
    std::string family() const override { return kFamily; }
    bool can_load(const rt::ModelLoadRequest &) const override { return false; }
    rt::ModelInspection inspect(const rt::ModelLoadRequest & request) const override {
        rt::ModelInspection inspection;
        inspection.metadata.family = kFamily;
        inspection.model_root = request.model_path;
        return inspection;
    }
    std::unique_ptr<rt::ILoadedVoiceModel> load(const rt::ModelLoadRequest &) const override {
        return std::make_unique<FakeModel>();
    }
};

struct TempRoot {
    fs::path path;
    TempRoot() {
        path = fs::temp_directory_path() /
            ("audio_cpp_workflow_named_audio_paths_" + std::to_string(std::random_device{}()));
        // Refuse a collision instead of deleting or reusing another test's files.
        require(fs::create_directory(path), "could not create isolated output directory");
    }
    ~TempRoot() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
    TempRoot(const TempRoot &) = delete;
    TempRoot & operator=(const TempRoot &) = delete;
};

void write_text(const fs::path & path, const std::string & text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
    require(out.good(), "failed to write test input " + path.string());
}

std::string read_bytes(const fs::path & path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "failed to open workflow output " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool same_path(const std::string & actual, const fs::path & expected) {
    return fs::path(actual).lexically_normal() == expected.lexically_normal();
}

void require_wav(const fs::path & path, float amplitude, const std::string & label) {
    require(fs::is_regular_file(path), label + ": missing WAV " + path.string());
    const std::string bytes = read_bytes(path);
    require(bytes.size() > 44, label + ": WAV too small");
    require(bytes.compare(0, 4, "RIFF") == 0 && bytes.compare(8, 4, "WAVE") == 0,
            label + ": not a RIFF/WAVE file");
    const auto wav = engine::audio::read_wav_f32(path);
    require(wav.sample_rate == kSampleRate, label + ": sample rate");
    require(wav.channels == 1, label + ": channel count");
    const auto expected = synthetic_samples(amplitude);
    require(wav.samples.size() == expected.size(), label + ": sample count");
    for (size_t i = 0; i < expected.size(); ++i) {
        require(std::fabs(wav.samples[i] - expected[i]) < 1.0e-3F,
                label + ": sample " + std::to_string(i) + " differs");
    }
}

engine::io::json::Value run_workflow(const TempRoot & root, const std::string & workflow_json) {
    fs::create_directories(root.path / "fake_model");
    const auto workflow_path = root.path / "workflow.json";
    write_text(workflow_path, workflow_json);

    rt::ModelRegistry registry;
    registry.register_loader(std::make_shared<FakeLoader>());
    minitts::app::WorkflowRunOptions options;
    options.workflow_path = workflow_path;
    options.output_dir = root.path / "out";
    minitts::app::run_json_workflow(registry, options);
    return engine::io::json::parse_file(root.path / "out" / "workflow_manifest.json");
}

std::string manifest_string(const engine::io::json::Value & manifest, const std::string & key) {
    const auto * value = manifest.find(key);
    require(value != nullptr, "workflow manifest missing " + key);
    return value->as_string();
}

// Every published <step>.*_path value must name a file that exists.
void require_published_paths_exist(const engine::io::json::Value & manifest, const std::string & step) {
    const std::string prefix = step + ".";
    const std::string suffix = "_path";
    for (const auto & [key, value] : manifest.as_object()) {
        if (key.rfind(prefix, 0) != 0 || key.size() < suffix.size() ||
            key.compare(key.size() - suffix.size(), suffix.size(), suffix) != 0) {
            continue;
        }
        require(fs::is_regular_file(fs::path(value.as_string())),
                "published " + key + " was never written: " + value.as_string());
    }
}

std::string model_step(const std::string & extra) {
    return R"({"id":"gen","type":"model","model":"fake_model","family":"fake_named_audio","task":"tts",)" + extra + "}";
}

void test_single_named_output_redirected_by_audio_out() {
    TempRoot root;
    const auto manifest = run_workflow(root, std::string(R"({"steps":[)") +
        model_step(R"("request":{"text":"alpha"},"audio_out":"${out_dir}/custom/redirected.wav")") + "," +
        R"({"id":"mix","type":"mix_audio","normalize_peak":false,"output":"${out_dir}/mix.wav","inputs":[)"
        R"({"path":"${gen.audio_path}","gain":0.5},{"path":"${gen.vocals_path}","gain":0.5}]}]})");

    const auto out = root.path / "out";
    const auto redirected = out / "custom" / "redirected.wav";
    const auto named = out / "gen" / "vocals.wav";
    require(same_path(manifest_string(manifest, "gen.audio_path"), redirected),
            "redirected: gen.audio_path must be the audio_out actually written");
    require(same_path(manifest_string(manifest, "gen.vocals_path"), named),
            "redirected: gen.vocals_path must be the named file written into the step directory");
    require_wav(redirected, 0.25F, "redirected audio_out");
    require_wav(named, 0.25F, "redirected named output");
    require(!fs::exists(out / "gen" / "gen.wav"), "redirected: default step WAV must not be written");
    require_published_paths_exist(manifest, "gen");
    require_wav(out / "mix.wav", 0.25F, "redirected mix of both aliases");
}

void test_single_named_output_default_audio_out() {
    TempRoot root;
    const auto manifest = run_workflow(root, std::string(R"({"steps":[)") +
        model_step(R"("request":{"text":"beta"})") + "," +
        R"({"id":"mix","type":"mix_audio","normalize_peak":false,"output":"${out_dir}/mix.wav","inputs":[)"
        R"({"path":"${gen.audio_path}","gain":0.5},{"path":"${gen.vocals_path}","gain":0.5}]}]})");

    const auto out = root.path / "out";
    require(same_path(manifest_string(manifest, "gen.audio_path"), out / "gen" / "gen.wav"),
            "default: gen.audio_path must be the default step WAV");
    require(same_path(manifest_string(manifest, "gen.vocals_path"), out / "gen" / "vocals.wav"),
            "default: gen.vocals_path must be the named step WAV");
    require_wav(out / "gen" / "gen.wav", 0.5F, "default audio_out");
    require_wav(out / "gen" / "vocals.wav", 0.5F, "default named output");
    require_published_paths_exist(manifest, "gen");
    require_wav(out / "mix.wav", 0.5F, "default mix of both aliases");
}

void test_foreach_items_publish_single_named_output_paths() {
    TempRoot root;
    fs::create_directories(root.path / "texts");
    write_text(root.path / "texts" / "alpha.txt", "alpha\n");
    write_text(root.path / "texts" / "gamma.txt", "gamma\n");
    run_workflow(root, std::string(R"({"steps":[)")
        + R"({"id":"texts","type":"batch_inputs","text_dir":"texts"},)"
        + model_step(R"("foreach":"${texts.items}","request":{"text":"${item.text}"})") + ","
        + R"({"id":"redir","type":"model","model":"fake_model","family":"fake_named_audio","task":"tts",)"
          R"("foreach":"${texts.items}","request":{"text":"${item.text}"},)"
          R"("audio_out":"${out_dir}/redirected/${item.id}.wav"},)"
        + R"({"id":"mix","type":"mix_audio","foreach":"${gen.items}","normalize_peak":false,)"
          R"("output":"${out_dir}/mixed/${item.id}.wav","inputs":[)"
          R"({"path":"${item.audio_path}","gain":0.5},{"path":"${item.vocals_path}","gain":0.5}]},)"
        + R"({"id":"mix_redir","type":"mix_audio","foreach":"${redir.items}","normalize_peak":false,)"
          R"("output":"${out_dir}/mixed_redir/${item.id}.wav","inputs":[)"
          R"({"path":"${item.audio_path}","gain":0.5},{"path":"${item.vocals_path}","gain":0.5}]}]})");

    const auto out = root.path / "out";
    const std::vector<std::pair<std::string, float>> items{{"alpha", 0.25F}, {"gamma", 0.75F}};
    for (const auto & [item, amplitude] : items) {
        require_wav(out / "gen" / item / "gen.wav", amplitude, "foreach default audio_out " + item);
        require_wav(out / "gen" / item / "vocals.wav", amplitude, "foreach default named " + item);
        require_wav(out / "mixed" / (item + ".wav"), amplitude, "foreach default mix " + item);

        require_wav(out / "redirected" / (item + ".wav"), amplitude, "foreach redirected audio_out " + item);
        require_wav(out / "redir" / item / "vocals.wav", amplitude, "foreach redirected named " + item);
        require(!fs::exists(out / "redir" / item / "redir.wav"),
                "foreach redirected: default step WAV must not be written for " + item);
        require_wav(out / "mixed_redir" / (item + ".wav"), amplitude, "foreach redirected mix " + item);
    }
}

void test_multiple_named_outputs_keep_only_named_aliases() {
    TempRoot root;
    const auto manifest = run_workflow(root, std::string(R"({"steps":[)") +
        model_step(R"("request":{"text":"multi:beta"})") + "]}");

    const auto out = root.path / "out";
    require(manifest.find("gen.audio_path") == nullptr,
            "multi: no generic audio_path when several named outputs exist");
    require(same_path(manifest_string(manifest, "gen.vocals_path"), out / "gen" / "vocals.wav"),
            "multi: vocals alias");
    require(same_path(manifest_string(manifest, "gen.accompaniment_path"), out / "gen" / "accompaniment.wav"),
            "multi: accompaniment alias");
    require_wav(out / "gen" / "vocals.wav", 0.5F, "multi vocals");
    require_wav(out / "gen" / "accompaniment.wav", 0.25F, "multi accompaniment");
    require(!fs::exists(out / "gen" / "gen.wav"), "multi: no default step WAV");
    require_published_paths_exist(manifest, "gen");
}

void test_primary_audio_output_paths_unchanged() {
    TempRoot root;
    const auto manifest = run_workflow(root, std::string(R"({"steps":[)") +
        model_step(R"("request":{"text":"primary:gamma"})") + "," +
        R"({"id":"redir","type":"model","model":"fake_model","family":"fake_named_audio","task":"tts",)"
        R"("request":{"text":"primary:alpha"},"audio_out":"${out_dir}/primary_redirected.wav"}]})");

    const auto out = root.path / "out";
    require(same_path(manifest_string(manifest, "gen.audio_path"), out / "gen" / "gen.wav"),
            "primary: default audio_path");
    require(same_path(manifest_string(manifest, "redir.audio_path"), out / "primary_redirected.wav"),
            "primary: redirected audio_path");
    require(manifest.find("gen.vocals_path") == nullptr, "primary: no named alias");
    require_wav(out / "gen" / "gen.wav", 0.75F, "primary default");
    require_wav(out / "primary_redirected.wav", 0.25F, "primary redirected");
    require_published_paths_exist(manifest, "gen");
    require_published_paths_exist(manifest, "redir");
}

}  // namespace

int main() {
    const std::vector<std::pair<const char *, void (*)()>> cases{
        {"test_single_named_output_redirected_by_audio_out", test_single_named_output_redirected_by_audio_out},
        {"test_single_named_output_default_audio_out", test_single_named_output_default_audio_out},
        {"test_foreach_items_publish_single_named_output_paths", test_foreach_items_publish_single_named_output_paths},
        {"test_multiple_named_outputs_keep_only_named_aliases", test_multiple_named_outputs_keep_only_named_aliases},
        {"test_primary_audio_output_paths_unchanged", test_primary_audio_output_paths_unchanged}
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
