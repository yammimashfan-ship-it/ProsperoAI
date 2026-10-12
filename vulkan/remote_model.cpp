// ProsperoAI - text models answered by another machine on the network.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "remote_model.hpp"
#include "net/http_post.h"
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <nlohmann/json.hpp>
#include <sys/stat.h>

extern "C" int sceKernelDebugOutText(int, const char *);

namespace prospero_remote
{
namespace
{
constexpr std::size_t kConfigBytes = 16 * 1024;
constexpr std::size_t kKeptBody = 16 * 1024;
constexpr std::size_t kLongestLine = 4 * 1024 * 1024;

void say(const char *format, ...)
{
    char line[512];
    std::va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(line, sizeof(line) - 1, format, arguments);
    va_end(arguments);
    std::strncat(line, "\n", sizeof(line) - std::strlen(line) - 1);
    sceKernelDebugOutText(0, line);
}

bool read_small_file(const std::string &path, std::string &text)
{
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (!file)
        return false;
    std::vector<char> buffer(kConfigBytes);
    const std::size_t size = std::fread(buffer.data(), 1, buffer.size(), file);
    std::fclose(file);
    text.assign(buffer.data(), size);
    return size > 0 && size < buffer.size();
}

std::string string_field(const nlohmann::json &object, const char *key)
{
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string();
}

bool ends_with(const std::string &text, const char *suffix)
{
    const std::size_t length = std::strlen(suffix);
    return text.size() >= length && text.compare(text.size() - length, length, suffix) == 0;
}

std::string chat_url(std::string url)
{
    while (!url.empty() && url.back() == '/')
        url.pop_back();
    if (ends_with(url, "/v1/chat/completions"))
        return url;
    if (ends_with(url, "/v1"))
        return url + "/chat/completions";
    return url + "/v1/chat/completions";
}

std::string error_message(const nlohmann::json &error)
{
    if (error.is_string())
        return error.get<std::string>();
    if (error.is_object())
    {
        const std::string message = string_field(error, "message");
        if (!message.empty())
            return message;
    }
    return "The server reported an error.";
}

struct Reply
{
    char *output = nullptr;
    std::size_t capacity = 0;
    std::size_t used = 0;
    gpt_runtime_progress_fn progress = nullptr;
    std::string line;  // the server-sent-events line being received
    std::string kept;  // the start of the body, for an answer that is not a stream
    std::string error; // an error the server reported in the stream
    unsigned pieces = 0;
    bool truncated = false;
    bool finished = false;
};

// Adds text to the reply, whole UTF-8 characters only, and shows it.
void append(Reply &reply, const std::string &text)
{
    if (text.empty() || reply.truncated)
        return;
    std::size_t length = text.size();
    if (reply.used + length + 1 > reply.capacity)
    {
        length = reply.capacity - 1 - reply.used;
        while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xc0) == 0x80)
            --length;
        reply.truncated = true;
    }
    std::memcpy(reply.output + reply.used, text.data(), length);
    reply.used += length;
    reply.output[reply.used] = 0;
    ++reply.pieces;
    if (reply.progress)
        reply.progress(reply.output);
}

// The content of an OpenAI chat answer or stream chunk, from its first choice.
void take_choice(Reply &reply, const nlohmann::json &answer)
{
    const auto error = answer.find("error");
    if (error != answer.end() && !error->is_null())
    {
        reply.error = error_message(*error);
        return;
    }
    const auto choices = answer.find("choices");
    if (choices == answer.end() || !choices->is_array() || choices->empty() ||
        !(*choices)[0].is_object())
        return;
    const auto &choice = (*choices)[0];
    for (const char *key : {"delta", "message"})
    {
        const auto part = choice.find(key);
        if (part != choice.end() && part->is_object())
            append(reply, string_field(*part, "content"));
    }
}

void take_line(Reply &reply, std::string line)
{
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    if (line.compare(0, 5, "data:") != 0)
        return; // a comment (keep-alive), an event name or the blank line between events
    std::size_t start = 5;
    while (start < line.size() && line[start] == ' ')
        ++start;
    if (line.compare(start, std::string::npos, "[DONE]") == 0)
    {
        reply.finished = true;
        return;
    }
    const auto chunk = nlohmann::json::parse(line.begin() + start, line.end(), nullptr, false);
    if (!chunk.is_discarded() && chunk.is_object())
        take_choice(reply, chunk);
}

std::size_t on_body(void *user, const char *data, std::size_t size)
{
    Reply &reply = *static_cast<Reply *>(user);
    // A stream is read line by line; a plain JSON answer is kept whole to be read at the end.
    const bool whole = !reply.kept.empty() ? reply.kept[0] == '{' : size && data[0] == '{';
    const std::size_t keep = whole ? kLongestLine : kKeptBody;
    if (reply.kept.size() < keep)
        reply.kept.append(data, std::min(size, keep - reply.kept.size()));
    for (std::size_t i = 0; i < size; ++i)
    {
        if (data[i] == '\n')
        {
            take_line(reply, reply.line);
            reply.line.clear();
        }
        else if (reply.line.size() < kLongestLine)
            reply.line.push_back(data[i]);
    }
    return size;
}

int fail(const Endpoint &endpoint, char *output, std::size_t capacity, const std::string &text)
{
    std::snprintf(output, capacity, "%s", text.c_str());
    say("[prosperoai] remote %s: %s", endpoint.id.c_str(), text.c_str());
    return 1;
}
} // namespace

std::vector<Endpoint> discover(const char *root)
{
    std::vector<Endpoint> found;
    DIR *directory = root ? opendir(root) : nullptr;
    if (!directory)
        return found;
    while (dirent *entry = readdir(directory))
    {
        const std::string name = entry->d_name;
        if (name.empty() || name[0] == '.')
            continue;
        const std::string folder = std::string(root) + "/" + name;
        struct stat info
        {
        };
        if (stat(folder.c_str(), &info) != 0 || !S_ISDIR(info.st_mode))
            continue;
        std::string text;
        if (!read_small_file(folder + "/remote.json", text))
            continue;
        const auto config = nlohmann::json::parse(text, nullptr, false);
        if (config.is_discarded() || !config.is_object())
        {
            say("[prosperoai] remote %s: remote.json is not a JSON object", name.c_str());
            continue;
        }
        Endpoint endpoint;
        endpoint.id = name;
        endpoint.name = string_field(config, "name");
        if (endpoint.name.empty())
            endpoint.name = name;
        endpoint.url = string_field(config, "url");
        if (endpoint.url.compare(0, 7, "http://") != 0 &&
            endpoint.url.compare(0, 8, "https://") != 0)
        {
            say("[prosperoai] remote %s: \"url\" must start with http:// or https://",
                name.c_str());
            continue;
        }
        endpoint.url = chat_url(endpoint.url);
        endpoint.token = string_field(config, "token");
        endpoint.model = string_field(config, "model");
        endpoint.system = string_field(config, "system");
        say("[prosperoai] remote %s: %s at %s", name.c_str(), endpoint.name.c_str(),
            endpoint.url.c_str());
        found.push_back(std::move(endpoint));
    }
    closedir(directory);
    std::sort(found.begin(), found.end(),
              [](const Endpoint &a, const Endpoint &b) { return a.name < b.name; });
    return found;
}

int generate(const Endpoint &endpoint, const gpt_runtime_message_t *messages, unsigned count,
             char *output, std::size_t capacity, gpt_runtime_stats_t *stats,
             gpt_runtime_progress_fn progress)
{
    if (!output || capacity < 2)
        return 1;
    output[0] = 0;
    if (stats)
        *stats = {};

    auto list = nlohmann::json::array();
    if (!endpoint.system.empty())
        list.push_back({{"role", "system"}, {"content", endpoint.system}});
    for (unsigned i = 0; i < count; ++i)
    {
        if (!messages[i].role || !messages[i].content)
            continue;
        const std::string role = messages[i].role;
        if (role != "user" && role != "assistant")
            continue;
        list.push_back({{"role", role}, {"content", messages[i].content}});
    }
    nlohmann::json request = {{"stream", true}, {"messages", std::move(list)}};
    if (!endpoint.model.empty())
        request["model"] = endpoint.model;
    const std::string body = request.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);

    Reply reply;
    reply.output = output;
    reply.capacity = capacity;
    reply.progress = progress;
    long status = 0;
    char error[256] = {};
    const auto start = std::chrono::steady_clock::now();
    const int failed =
        prospero_http_post(endpoint.url.c_str(), endpoint.token.c_str(), body.data(), body.size(),
                           on_body, &reply, &status, error, sizeof(error));
    if (!reply.line.empty())
        take_line(reply, reply.line);
    if (stats)
    {
        stats->generated_tokens = reply.pieces;
        stats->output_limit_reached = reply.truncated;
        stats->elapsed_microseconds =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                           std::chrono::steady_clock::now() - start)
                                           .count());
    }

    if (failed && reply.used == 0)
        return fail(endpoint, output, capacity,
                    "Couldn't reach " + endpoint.name + " (" + error +
                        "). Check that claude-bridge is running on your PC, that the PC and "
                        "the PS5 are on the same network, and the address in remote.json.");
    if (status == 401)
        return fail(endpoint, output, capacity,
                    endpoint.name +
                        " rejected the token. Copy remote.json from the bridge's ps5-model "
                        "folder to the PS5 again.");
    if (status < 200 || status >= 300)
    {
        std::string message;
        const auto answer = nlohmann::json::parse(reply.kept, nullptr, false);
        if (!answer.is_discarded() && answer.is_object() && answer.contains("error"))
            message = error_message(answer["error"]);
        if (message.empty())
            message = "the server answered HTTP " + std::to_string(status) + ".";
        return fail(endpoint, output, capacity, endpoint.name + ": " + message);
    }
    if (reply.pieces == 0 && reply.error.empty() && !reply.kept.empty() && reply.kept[0] == '{')
    {
        // A server that ignored "stream" and answered with one JSON object.
        const auto answer = nlohmann::json::parse(reply.kept, nullptr, false);
        if (!answer.is_discarded() && answer.is_object())
            take_choice(reply, answer);
    }
    if (reply.used == 0)
        return fail(endpoint, output, capacity,
                    reply.error.empty() ? endpoint.name + " sent an empty reply."
                                        : endpoint.name + ": " + reply.error);
    if (!reply.error.empty() || failed)
        say("[prosperoai] remote %s: reply ended early: %s", endpoint.id.c_str(),
            reply.error.empty() ? error : reply.error.c_str());
    return 0;
}
} // namespace prospero_remote
