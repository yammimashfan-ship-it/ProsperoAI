// SPDX-License-Identifier: GPL-3.0-or-later
// vulkan/remote_model.cpp on the host: remote.json discovery and the streamed chat reply,
// with prospero_http_post replaced by canned answers delivered in pieces of every size.
#include "remote_model.hpp"
#include "net/http_post.h"
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace
{
struct Canned
{
    int failed = 0;
    long status = 200;
    std::string body;
    std::size_t piece = 1; // bytes per call of the body callback
    const char *error = "";
} canned;

std::string sent_url, sent_bearer, sent_body;
std::vector<std::string> shown;

void progress(const char *text)
{
    shown.emplace_back(text);
}

bool valid_utf8(const std::string &text)
{
    for (std::size_t i = 0; i < text.size();)
    {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        const std::size_t length = lead < 0x80   ? 1
                                   : lead < 0xc0 ? 0
                                   : lead < 0xe0 ? 2
                                   : lead < 0xf0 ? 3
                                                 : 4;
        if (!length || i + length > text.size())
            return false;
        for (std::size_t j = 1; j < length; ++j)
            if ((static_cast<unsigned char>(text[i + j]) & 0xc0) != 0x80)
                return false;
        i += length;
    }
    return true;
}

std::string sse(const std::vector<std::string> &pieces, bool crlf = false)
{
    const std::string end = crlf ? "\r\n" : "\n";
    std::string body =
        "data: " + nlohmann::json{{"choices", {{{"delta", {{"role", "assistant"}}}}}}}.dump() +
        end + end + ": keepalive" + end + end;
    for (const auto &piece : pieces)
        body += "data: " + nlohmann::json{{"choices", {{{"delta", {{"content", piece}}}}}}}.dump() +
                end + end;
    body += "data: " +
            nlohmann::json{
                {"choices", {{{"delta", nlohmann::json::object()}, {"finish_reason", "stop"}}}}}
                .dump() +
            end + end + "data: [DONE]" + end + end;
    return body;
}

prospero_remote::Endpoint endpoint()
{
    prospero_remote::Endpoint e;
    e.id = "claude-pc";
    e.name = "Claude (PC)";
    e.url = "http://192.168.1.20:8765/v1/chat/completions";
    e.token = "abcd-efgh";
    e.model = "claude";
    return e;
}

const gpt_runtime_message_t conversation[] = {
    {"system", "You are ProsperoAI, running locally on a PlayStation 5."},
    {"user", "Hi"},
    {"assistant", "Hello!"},
    {"user", "Tell me about the PS5 \xf0\x9f\x8e\xae"},
};

int run(const prospero_remote::Endpoint &e, char *out, std::size_t capacity,
        gpt_runtime_stats_t *stats = nullptr)
{
    shown.clear();
    return prospero_remote::generate(e, conversation, 4, out, capacity, stats, progress);
}
} // namespace

extern "C"
{
    int sceKernelDebugOutText(int, const char *)
    {
        return 0;
    }

    int prospero_http_post(const char *url, const char *bearer, const char *body,
                           std::size_t body_size, prospero_http_body_fn on_body, void *user,
                           long *status, char *error, std::size_t error_size)
    {
        sent_url = url;
        sent_bearer = bearer ? bearer : "";
        sent_body.assign(body, body_size);
        for (std::size_t at = 0; at < canned.body.size(); at += canned.piece)
        {
            const std::size_t size = std::min(canned.piece, canned.body.size() - at);
            assert(on_body(user, canned.body.data() + at, size) == size);
        }
        *status = canned.failed && canned.body.empty() ? 0 : canned.status;
        std::snprintf(error, error_size, "%s", canned.error);
        return canned.failed;
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    namespace fs = std::filesystem;
    const fs::path root = argv[1];
    auto write = [&](const char *folder, const std::string &text)
    {
        fs::create_directories(root / folder);
        std::ofstream(root / folder / "remote.json") << text;
    };
    write("claude-pc", R"json({"name":"Claude (PC)","url":"http://192.168.1.20:8765/",)json"
                       R"json("token":"abcd-efgh","model":"claude"})json");
    write("ollama", R"({"url":"http://192.168.1.30:11434/v1","system":"Be brief."})");
    write("broken", "{not json");
    write("no-url", R"({"name":"Nowhere"})");
    write("ftp", R"({"name":"FTP","url":"ftp://192.168.1.20/"})");
    fs::create_directories(root / "mistral-7b");
    std::ofstream(root / "mistral-7b" / "model.gguf") << "weights";
    std::ofstream(root / "remote.json") << R"({"url":"http://loose.file/"})";

    // Discovery: only folders with a usable remote.json, sorted by name.
    const auto found = prospero_remote::discover(root.c_str());
    assert(found.size() == 2);
    assert(found[0].id == "claude-pc" && found[0].name == "Claude (PC)");
    assert(found[0].url == "http://192.168.1.20:8765/v1/chat/completions");
    assert(found[0].token == "abcd-efgh" && found[0].model == "claude");
    assert(found[1].id == "ollama" && found[1].name == "ollama");
    assert(found[1].url == "http://192.168.1.30:11434/v1/chat/completions");
    assert(found[1].system == "Be brief." && found[1].token.empty());
    assert(prospero_remote::discover((root / "missing").c_str()).empty());
    assert(prospero_remote::discover(nullptr).empty());

    char out[4096];
    gpt_runtime_stats_t stats{};
    const std::string reply = "The PS5 \xf0\x9f\x8e\xae has a custom AMD chip. "
                              "Caf\xc3\xa9 \xe2\x9c\x93";

    // A streamed reply arrives whole whatever the size of the pieces it is read in.
    for (std::size_t piece : {1, 2, 3, 7, 64, 100000})
        for (bool crlf : {false, true})
        {
            canned = {};
            canned.piece = piece;
            canned.body = sse({"The PS5 \xf0\x9f\x8e\xae has", " a custom AMD chip. ",
                               "Caf\xc3\xa9 \xe2\x9c\x93"},
                              crlf);
            assert(run(endpoint(), out, sizeof(out), &stats) == 0);
            assert(out == reply);
            assert(stats.generated_tokens == 3 && !stats.output_limit_reached);
            assert(shown.size() == 3 && shown.back() == reply);
            assert(shown[0] == "The PS5 \xf0\x9f\x8e\xae has");
        }

    // The request: the app's own system prompt stays home, the conversation goes out.
    assert(sent_url == "http://192.168.1.20:8765/v1/chat/completions");
    assert(sent_bearer == "abcd-efgh");
    const auto request = nlohmann::json::parse(sent_body);
    assert(request["stream"] == true && request["model"] == "claude");
    assert(request["messages"].size() == 3);
    assert(request["messages"][0]["role"] == "user" && request["messages"][0]["content"] == "Hi");
    assert(request["messages"][1]["role"] == "assistant");
    assert(request["messages"][2]["content"] == "Tell me about the PS5 \xf0\x9f\x8e\xae");

    // remote.json's own system prompt and no model or token.
    canned = {};
    canned.body = sse({"ok"});
    assert(run(found[1], out, sizeof(out)) == 0 && std::string(out) == "ok");
    const auto second = nlohmann::json::parse(sent_body);
    assert(!second.contains("model") && sent_bearer.empty());
    assert(second["messages"][0]["role"] == "system");
    assert(second["messages"][0]["content"] == "Be brief.");

    // A reply longer than the buffer stops at a whole character and says so.
    for (std::size_t capacity = 2; capacity < 24; ++capacity)
    {
        canned = {};
        canned.body = sse({reply});
        std::vector<char> small(capacity);
        assert(run(endpoint(), small.data(), small.size(), &stats) == 0 || capacity < 6);
        const std::string got = small.data();
        assert(got.size() < capacity && valid_utf8(got));
        assert(reply.compare(0, got.size(), got) == 0);
        if (!got.empty())
            assert(stats.output_limit_reached);
    }

    // An error in the stream before any text fails with the server's message.
    canned = {};
    canned.body = "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}\n\n"
                  "data: {\"error\":{\"message\":\"Claude Code on the PC isn't signed in.\"}}\n\n"
                  "data: [DONE]\n\n";
    assert(run(endpoint(), out, sizeof(out)) == 1);
    assert(std::string(out) == "Claude (PC): Claude Code on the PC isn't signed in.");

    // An error after some text keeps the text.
    canned = {};
    canned.body = sse({"Partial answer"});
    canned.body.insert(canned.body.find("data: [DONE]"),
                       "data: {\"error\":{\"message\":\"cut off\"}}\n\n");
    assert(run(endpoint(), out, sizeof(out)) == 0 && std::string(out) == "Partial answer");

    // Wrong token.
    canned = {};
    canned.status = 401;
    canned.body = R"({"error":{"message":"Wrong or missing token."}})";
    assert(run(endpoint(), out, sizeof(out)) == 1);
    assert(std::string(out).find("rejected the token") != std::string::npos);

    // Another HTTP error with a JSON body, and one without.
    canned = {};
    canned.status = 429;
    canned.body = R"({"error":{"message":"Claude is busy with other replies."}})";
    assert(run(endpoint(), out, sizeof(out)) == 1);
    assert(std::string(out) == "Claude (PC): Claude is busy with other replies.");
    canned.status = 500;
    canned.body = "<html>oops</html>";
    assert(run(endpoint(), out, sizeof(out)) == 1);
    assert(std::string(out) == "Claude (PC): the server answered HTTP 500.");

    // No answer at all: the bridge is not running.
    canned = {};
    canned.failed = 1;
    canned.error = "Couldn't connect to server";
    assert(run(endpoint(), out, sizeof(out)) == 1);
    assert(std::string(out).find("Couldn't reach Claude (PC) (Couldn't connect to server)") == 0);

    // The connection drops mid-reply: what arrived is kept.
    canned = {};
    canned.failed = 1;
    canned.error = "Operation timed out";
    canned.body = "data: {\"choices\":[{\"delta\":{\"content\":\"Half a th\"}}]}\n\n";
    assert(run(endpoint(), out, sizeof(out)) == 0 && std::string(out) == "Half a th");

    // A server that ignores "stream" and answers with one JSON object, in pieces.
    canned = {};
    canned.piece = 5;
    canned.body =
        nlohmann::json{{"choices", {{{"message", {{"role", "assistant"}, {"content", reply}}}}}}}
            .dump();
    assert(run(endpoint(), out, sizeof(out)) == 0 && out == reply);

    // A stream with nothing in it.
    canned = {};
    canned.body = sse({});
    assert(run(endpoint(), out, sizeof(out)) == 1);
    assert(std::string(out) == "Claude (PC) sent an empty reply.");

    // Lines without a trailing newline at the end of the body are still read.
    canned = {};
    canned.body = "data: {\"choices\":[{\"delta\":{\"content\":\"last line\"}}]}";
    assert(run(endpoint(), out, sizeof(out)) == 0 && std::string(out) == "last line");

    std::puts("remote model: all checks passed");
    return 0;
}
